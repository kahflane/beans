#include "interp.h"

#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <utility>

#include "lexer.h"
#include "parser.h"

namespace beans {

namespace {

// per-thread frame state: defers + the return-type hint of the running fn
struct Frame {
    std::vector<std::pair<const Expr*, std::shared_ptr<Env>>> defers;
    const TypeRef* ret = nullptr;
};

thread_local std::vector<Frame> g_frames;

int64_t parse_int_text(std::string_view text) {
    std::string clean;
    for (char c : text) {
        if (c != '_') clean.push_back(c);
    }
    if (clean.size() > 2 && clean[0] == '0' && (clean[1] == 'x' || clean[1] == 'X')) {
        return static_cast<int64_t>(std::strtoull(clean.c_str() + 2, nullptr, 16));
    }
    if (clean.size() > 2 && clean[0] == '0' && (clean[1] == 'b' || clean[1] == 'B')) {
        return static_cast<int64_t>(std::strtoull(clean.c_str() + 2, nullptr, 2));
    }
    return static_cast<int64_t>(std::strtoll(clean.c_str(), nullptr, 10));
}

std::string clean_number(std::string_view text) {
    std::string out;
    for (char c : text) {
        if (c != '_') out.push_back(c);
    }
    return out;
}

} // namespace

// ---- hints -----------------------------------------------------------------

Interp::Hint Interp::Hint::of(const TypeRef* t) {
    Hint h;
    h.tref = t;
    if (t && t->kind == TypeRef::Kind::named) {
        if (t->name == "decimal") h.num = NumHint::dec;
        else if (t->name == "float" || t->name == "f64" || t->name == "f32")
            h.num = NumHint::flt;
    }
    return h;
}
bool Interp::Hint::wants_dec() const { return num == NumHint::dec; }
bool Interp::Hint::wants_float() const { return num == NumHint::flt; }
const TypeRef* Interp::Hint::arg(size_t i) const {
    if (tref && tref->kind == TypeRef::Kind::named && i < tref->args.size()) {
        return tref->args[i].get();
    }
    return nullptr;
}

// ---- setup -----------------------------------------------------------------

// which package's code this thread is currently running (parallel to g_frames)
thread_local std::string g_pkg;

Interp::Interp(const Program& prog) {
    for (const auto& pkg : prog.packages) {
        prefix_by_path_[pkg->import_path] = pkg->prefix;
        auto& bindings = pkg_imports_[pkg->prefix];
        for (const auto& pf : pkg->files) {
            for (const ClassDecl& c : pf->mod.classes) classes_[c.qualname] = &c;
            for (const EnumDecl& e : pf->mod.enums) enums_[e.qualname] = &e;
            for (const FnDecl& f : pf->mod.fns) fns_[f.qualname] = &f;
            for (const ImportDecl& i : pf->mod.imports) {
                std::string bound = i.alias;
                if (bound.empty()) {
                    size_t cut = i.path.find_last_of("./");
                    bound = cut == std::string::npos ? i.path : i.path.substr(cut + 1);
                }
                bindings[bound] = i.path;
            }
        }
    }
}

std::string Interp::qual(const std::string& name) const {
    return g_pkg.empty() ? name : g_pkg + "." + name;
}

std::string Interp::pkg_of(const std::string& qualname) {
    size_t dot = qualname.find('.');
    return dot == std::string::npos ? "" : qualname.substr(0, dot);
}

const ClassDecl* Interp::resolve_class(const Expr* annotated, const std::string& name) const {
    if (annotated && !annotated->resolved.empty()) return find_class(annotated->resolved);
    if (const ClassDecl* c = find_class(qual(name))) return c;
    return nullptr;
}

const EnumDecl* Interp::resolve_enum(const Expr* annotated, const std::string& name) const {
    const std::string& key = annotated && !annotated->resolved.empty()
                                 ? annotated->resolved
                                 : qual(name);
    auto it = enums_.find(key);
    return it == enums_.end() ? nullptr : it->second;
}

std::string Interp::binding_path(const std::string& binding) const {
    auto pit = pkg_imports_.find(g_pkg);
    if (pit == pkg_imports_.end()) return "";
    auto it = pit->second.find(binding);
    return it == pit->second.end() ? "" : it->second;
}

int Interp::run() {
    auto it = fns_.find("main");
    if (it == fns_.end()) {
        std::fprintf(stderr, "error: no fn main\n");
        return 2;
    }
    try {
        call_fn(it->second, nullptr, {}, "");
    } catch (const BeansPanic& p) {
        std::fprintf(stderr, "runtime panic at %u:%u: %s\n", p.line, p.col, p.msg.c_str());
        return 3;
    }
    return 0;
}

void Interp::panic(const Expr* e, std::string msg) {
    BeansPanic p;
    p.msg = std::move(msg);
    if (e) { p.line = e->line; p.col = e->col; }
    throw p;
}

Value Interp::some(Value v) {
    Value x;
    x.k = Value::K::enum_v;
    x.en = std::make_shared<EnumVal>();
    x.en->enum_name = "Option";
    x.en->variant = "some";
    x.en->payload.push_back(std::move(v));
    return x;
}
Value Interp::none() {
    Value x;
    x.k = Value::K::enum_v;
    x.en = std::make_shared<EnumVal>();
    x.en->enum_name = "Option";
    x.en->variant = "none";
    return x;
}
Value Interp::make_err(std::string msg) {
    Value e;
    e.k = Value::K::instance;
    e.inst = std::make_shared<InstanceVal>();
    e.inst->fields.emplace_back("msg", Value::of_str(std::move(msg)));
    e.inst->fields.emplace_back("kind", Value::of_str(""));
    return e;
}

// ---- class helpers ----------------------------------------------------------

const ClassDecl* Interp::find_class(const std::string& name) const {
    auto it = classes_.find(name);
    return it == classes_.end() ? nullptr : it->second;
}

// parents as qualified keys (the checker pinned them); raw names as fallback
static const std::vector<std::string>& supers_of(const ClassDecl* c) {
    return c->supers_resolved.empty() ? c->supers : c->supers_resolved;
}

const FnDecl* Interp::find_method(const ClassDecl* cls, const std::string& name,
                                  const ClassDecl** owner) const {
    std::vector<const ClassDecl*> work = {cls};
    std::vector<const ClassDecl*> seen;
    while (!work.empty()) {
        const ClassDecl* c = work.front();
        work.erase(work.begin());
        if (!c) continue;
        bool dup = false;
        for (const ClassDecl* s : seen) dup |= s == c;
        if (dup) continue;
        seen.push_back(c);
        for (const FnDecl& m : c->methods) {
            if (m.name == name && m.has_body) {
                if (owner) *owner = c; // inherited code runs as its own package
                return &m;
            }
        }
        for (const std::string& s : supers_of(c)) work.push_back(find_class(s));
    }
    return nullptr;
}

bool Interp::class_is(const ClassDecl* cls, const std::string& super) const {
    if (!cls) return false;
    if (cls->qualname == super) return true;
    for (const std::string& s : supers_of(cls)) {
        if (class_is(find_class(s), super)) return true;
    }
    return false;
}

void Interp::collect_fields(const ClassDecl* cls, std::vector<const FieldDecl*>& out) const {
    if (!cls) return;
    for (const std::string& s : supers_of(cls)) collect_fields(find_class(s), out);
    for (const FieldDecl& f : cls->fields) {
        bool have = false;
        for (const FieldDecl* g : out) have |= g->name == f.name;
        if (!have) out.push_back(&f);
    }
}

Value Interp::make_instance(const ClassDecl* cls,
                            const std::vector<InitEntry>& entries,
                            std::shared_ptr<Env>& env) {
    Value v;
    v.k = Value::K::instance;
    v.inst = std::make_shared<InstanceVal>();
    v.inst->cls = cls;

    std::vector<const FieldDecl*> fields;
    collect_fields(cls, fields);
    std::string saved_pkg = g_pkg;
    g_pkg = pkg_of(cls->qualname); // defaults are the class's own code
    for (const FieldDecl* f : fields) {
        Value init = Value::unit();
        if (f->def) {
            std::shared_ptr<Env> empty = std::make_shared<Env>();
            init = eval(f->def.get(), empty, Hint::of(f->type.get()));
        }
        v.inst->fields.emplace_back(f->name, std::move(init));
    }
    g_pkg = saved_pkg;
    for (const InitEntry& en : entries) {
        const FieldDecl* fd = nullptr;
        for (const FieldDecl* f : fields) {
            if (f->name == en.name) fd = f;
        }
        Value val = eval(en.value.get(), env, fd ? Hint::of(fd->type.get()) : Hint());
        if (Value* slot = v.inst->field(en.name)) *slot = std::move(val);
    }
    return v;
}

// ---- calls -----------------------------------------------------------------

Value Interp::call_fn(const FnDecl* fn, Value* self, std::vector<Value> args,
                      const std::string& pkg) {
    auto env = std::make_shared<Env>();
    if (self) env->declare("self", *self);
    for (size_t i = 0; i < fn->params.size() && i < args.size(); i++) {
        env->declare(fn->params[i].name, std::move(args[i]));
    }

    std::string saved_pkg = g_pkg;
    g_pkg = pkg;
    g_frames.push_back({});
    g_frames.back().ret = fn->ret.get();
    Value result = Value::unit();
    try {
        exec_block(fn->body, env);
    } catch (ReturnSignal& r) {
        result = std::move(r.v);
    } catch (...) {
        auto defers = std::move(g_frames.back().defers);
        g_frames.pop_back();
        for (auto it = defers.rbegin(); it != defers.rend(); ++it) {
            try { eval(it->first, it->second); } catch (...) {}
        }
        g_pkg = saved_pkg;
        throw;
    }
    auto defers = std::move(g_frames.back().defers);
    g_frames.pop_back();
    for (auto it = defers.rbegin(); it != defers.rend(); ++it) {
        try {
            eval(it->first, it->second);
        } catch (const BeansPanic& p) {
            std::fprintf(stderr, "panic in defer (ignored): %s\n", p.msg.c_str());
        } catch (...) {}
    }
    g_pkg = saved_pkg;
    return result;
}

Value Interp::call_closure(const ClosureVal& c, std::vector<Value> args) {
    const Expr* node = c.node;
    auto env = std::make_shared<Env>();
    env->parent = c.captured;
    for (size_t i = 0; i < node->params.size() && i < args.size(); i++) {
        env->declare(node->params[i].name, std::move(args[i]));
    }

    std::string saved_pkg = g_pkg;
    g_pkg = c.pkg; // the closure runs as code of the package that wrote it
    g_frames.push_back({});
    g_frames.back().ret = node->type.get();
    Value result = Value::unit();
    try {
        exec_block(node->body, env);
    } catch (ReturnSignal& r) {
        result = std::move(r.v);
    } catch (...) {
        g_frames.pop_back();
        g_pkg = saved_pkg;
        throw;
    }
    auto defers = std::move(g_frames.back().defers);
    g_frames.pop_back();
    for (auto it = defers.rbegin(); it != defers.rend(); ++it) {
        try { eval(it->first, it->second); } catch (...) {}
    }
    g_pkg = saved_pkg;
    return result;
}

// ---- statements ------------------------------------------------------------

void Interp::exec_block(const std::vector<StmtPtr>& body, std::shared_ptr<Env> env) {
    auto scope = std::make_shared<Env>();
    scope->parent = env;
    for (const StmtPtr& s : body) exec_stmt(s.get(), scope);
}

void Interp::exec_stmt(const Stmt* s, std::shared_ptr<Env>& env) {
    switch (s->kind) {
        case Stmt::Kind::let_: {
            Value v = s->init ? eval(s->init.get(), env, Hint::of(s->type.get()))
                              : Value::unit();
            env->declare(s->name, std::move(v));
            break;
        }
        case Stmt::Kind::assign: {
            if (s->op == TokenKind::assign) {
                // hint from the current slot so decimal stays decimal
                Value* slot = lvalue_slot(s->target.get(), env);
                Hint h;
                if (slot && slot->k == Value::K::decimal_) h = Hint::decimal();
                if (slot && slot->k == Value::K::float_) h = Hint::floating();
                Value v = eval(s->value.get(), env, h);
                Value* slot2 = lvalue_slot(s->target.get(), env);
                if (slot2) *slot2 = std::move(v);
                break;
            }
            Value* slot = lvalue_slot(s->target.get(), env);
            if (!slot) break;
            Hint h;
            if (slot->k == Value::K::decimal_) h = Hint::decimal();
            if (slot->k == Value::K::float_) h = Hint::floating();
            Value rhs = eval(s->value.get(), env, h);
            Value* slot2 = lvalue_slot(s->target.get(), env);
            if (!slot2) break;
            switch (slot2->k) {
                case Value::K::int_:
                    switch (s->op) {
                        case TokenKind::plus_eq: slot2->i += rhs.i; break;
                        case TokenKind::minus_eq: slot2->i -= rhs.i; break;
                        case TokenKind::star_eq: slot2->i *= rhs.i; break;
                        case TokenKind::slash_eq:
                            if (rhs.i == 0) panic(s->value.get(), "divide by zero");
                            slot2->i /= rhs.i;
                            break;
                        case TokenKind::percent_eq:
                            if (rhs.i == 0) panic(s->value.get(), "modulo by zero");
                            slot2->i %= rhs.i;
                            break;
                        default: break;
                    }
                    break;
                case Value::K::float_:
                    switch (s->op) {
                        case TokenKind::plus_eq: slot2->f += rhs.f; break;
                        case TokenKind::minus_eq: slot2->f -= rhs.f; break;
                        case TokenKind::star_eq: slot2->f *= rhs.f; break;
                        case TokenKind::slash_eq: slot2->f /= rhs.f; break;
                        default: break;
                    }
                    break;
                case Value::K::decimal_:
                    switch (s->op) {
                        case TokenKind::plus_eq: slot2->dec = slot2->dec.add(rhs.dec); break;
                        case TokenKind::minus_eq: slot2->dec = slot2->dec.sub(rhs.dec); break;
                        case TokenKind::star_eq: slot2->dec = slot2->dec.mul(rhs.dec); break;
                        case TokenKind::slash_eq:
                            if (rhs.dec.coeff == 0) panic(s->value.get(), "divide by zero");
                            slot2->dec = slot2->dec.div(rhs.dec);
                            break;
                        default: break;
                    }
                    break;
                default:
                    break;
            }
            break;
        }
        case Stmt::Kind::expr:
            eval(s->expr.get(), env);
            break;
        case Stmt::Kind::ret: {
            ReturnSignal r;
            const TypeRef* rt = g_frames.empty() ? nullptr : g_frames.back().ret;
            r.v = s->expr ? eval(s->expr.get(), env, Hint::of(rt)) : Value::unit();
            throw r;
        }
        case Stmt::Kind::brk:
            throw BreakSignal{};
        case Stmt::Kind::cont:
            throw ContinueSignal{};
        case Stmt::Kind::if_: {
            Value c = eval(s->cond.get(), env);
            if (c.b) exec_block(s->body, env);
            else if (!s->else_body.empty()) exec_block(s->else_body, env);
            break;
        }
        case Stmt::Kind::for_ever: {
            while (true) {
                try {
                    exec_block(s->body, env);
                } catch (BreakSignal&) {
                    break;
                } catch (ContinueSignal&) {
                    continue;
                }
            }
            break;
        }
        case Stmt::Kind::for_while: {
            while (true) {
                Value c = eval(s->cond.get(), env);
                if (!c.b) break;
                try {
                    exec_block(s->body, env);
                } catch (BreakSignal&) {
                    break;
                } catch (ContinueSignal&) {
                    continue;
                }
            }
            break;
        }
        case Stmt::Kind::for_in: {
            Value it = eval(s->iterable.get(), env);
            auto run_iter = [&](Value item) -> bool {
                auto iter_env = std::make_shared<Env>();
                iter_env->parent = env;
                iter_env->declare(s->loop_var, std::move(item));
                try {
                    exec_block(s->body, iter_env);
                } catch (BreakSignal&) {
                    return false;
                } catch (ContinueSignal&) {
                }
                return true;
            };
            if (it.k == Value::K::range) {
                int64_t hi = it.range->hi + (it.range->inclusive ? 1 : 0);
                for (int64_t x = it.range->lo; x < hi; x++) {
                    if (!run_iter(Value::of_int(x))) break;
                }
            } else if (it.k == Value::K::list) {
                for (size_t idx = 0; idx < it.list->items.size(); idx++) {
                    if (!run_iter(it.list->items[idx])) break;
                }
            }
            break;
        }
        case Stmt::Kind::defer_:
            if (!g_frames.empty()) {
                g_frames.back().defers.emplace_back(s->expr.get(), env);
            }
            break;
        case Stmt::Kind::unsafe_:
            exec_block(s->body, env);
            break;
    }
}

// ---- lvalues ---------------------------------------------------------------

Value* Interp::lvalue_slot(const Expr* target, std::shared_ptr<Env>& env) {
    if (target->kind == Expr::Kind::ident) {
        return env->find(std::string(target->text));
    }
    if (target->kind == Expr::Kind::field) {
        Value obj = eval(target->object.get(), env);
        if (obj.k == Value::K::instance) {
            // instance is shared — the slot stays valid after obj dies
            return obj.inst->field(target->name);
        }
        panic(target, "can't assign to this field");
    }
    if (target->kind == Expr::Kind::index) {
        Value obj = eval(target->object.get(), env);
        Value idx = eval(target->index_expr.get(), env);
        if (obj.k == Value::K::list) {
            int64_t i = idx.i;
            if (i < 0 || static_cast<size_t>(i) >= obj.list->items.size()) {
                panic(target, "list index " + std::to_string(i) + " out of range");
            }
            return &obj.list->items[static_cast<size_t>(i)];
        }
        if (obj.k == Value::K::map) {
            for (auto& [k, v] : obj.map->entries) {
                if (value_eq(k, idx)) return &v;
            }
            obj.map->entries.emplace_back(idx, Value::unit());
            return &obj.map->entries.back().second;
        }
    }
    panic(target, "can't assign here");
}

void Interp::assign_to(const Expr* target, Value v, std::shared_ptr<Env>& env) {
    if (Value* slot = lvalue_slot(target, env)) *slot = std::move(v);
}

// ---- expressions -----------------------------------------------------------

Value Interp::eval(const Expr* e, std::shared_ptr<Env>& env, Hint hint) {
    switch (e->kind) {
        case Expr::Kind::int_lit: {
            if (hint.wants_dec()) return Value::of_dec(Decimal::parse(clean_number(e->text)));
            if (hint.wants_float())
                return Value::of_float(std::strtod(clean_number(e->text).c_str(), nullptr));
            return Value::of_int(parse_int_text(e->text));
        }
        case Expr::Kind::float_lit: {
            if (hint.wants_dec()) return Value::of_dec(Decimal::parse(clean_number(e->text)));
            return Value::of_float(std::strtod(clean_number(e->text).c_str(), nullptr));
        }
        case Expr::Kind::string_lit:
            return eval_string(e, env);
        case Expr::Kind::bool_lit:
            return Value::of_bool(e->bool_val);
        case Expr::Kind::ident: {
            std::string name(e->text);
            if (Value* v = env->find(name)) return *v;
            if (name == "none") return none();
            auto fit = fns_.find(e->resolved.empty() ? qual(name) : e->resolved);
            if (fit != fns_.end()) {
                Value v;
                v.k = Value::K::fn_ref;
                v.fnr = std::make_shared<FnRefVal>();
                v.fnr->decl = fit->second;
                return v;
            }
            panic(e, "unknown name '" + name + "'");
        }
        case Expr::Kind::self_ref: {
            if (Value* v = env->find("self")) return *v;
            panic(e, "self not bound");
        }
        case Expr::Kind::unary: {
            if (e->op == TokenKind::minus) {
                Value v = eval(e->rhs.get(), env, hint);
                switch (v.k) {
                    case Value::K::int_: v.i = -v.i; return v;
                    case Value::K::float_: v.f = -v.f; return v;
                    case Value::K::decimal_: v.dec = v.dec.neg(); return v;
                    default: return v;
                }
            }
            if (e->op == TokenKind::bang) {
                Value v = eval(e->rhs.get(), env);
                return Value::of_bool(!v.b);
            }
            Value v = eval(e->rhs.get(), env); // ~
            v.i = ~v.i;
            return v;
        }
        case Expr::Kind::binary:
            return eval_binary(e, env);
        case Expr::Kind::range: {
            Value lo = eval(e->lhs.get(), env);
            Value hi = eval(e->rhs.get(), env);
            Value v;
            v.k = Value::K::range;
            v.range = std::make_shared<RangeVal>();
            v.range->lo = lo.i;
            v.range->hi = hi.i;
            v.range->inclusive = e->inclusive;
            return v;
        }
        case Expr::Kind::call:
            return eval_call(e, env, hint);
        case Expr::Kind::field: {
            const Expr* obj = e->object.get();
            if (obj->kind == Expr::Kind::ident && !env->find(std::string(obj->text))) {
                std::string n(obj->text);
                if (const EnumDecl* ed = resolve_enum(obj, n)) {
                    Value v;
                    v.k = Value::K::enum_v;
                    v.en = std::make_shared<EnumVal>();
                    v.en->enum_name = ed->qualname;
                    v.en->variant = e->name;
                    return v;
                }
            }
            if (obj->kind == Expr::Kind::field && !obj->resolved.empty()) {
                // `util.Status.active` — the checker pinned the enum
                if (const EnumDecl* ed = resolve_enum(obj, "")) {
                    Value v;
                    v.k = Value::K::enum_v;
                    v.en = std::make_shared<EnumVal>();
                    v.en->enum_name = ed->qualname;
                    v.en->variant = e->name;
                    return v;
                }
            }
            Value v = eval(obj, env);
            if (v.k == Value::K::instance) {
                if (Value* f = v.inst->field(e->name)) return *f;
                panic(e, "no field '" + e->name + "'");
            }
            panic(e, "no field '" + e->name + "' on this value");
        }
        case Expr::Kind::index: {
            Value obj = eval(e->object.get(), env);
            Value idx = eval(e->index_expr.get(), env);
            if (obj.k == Value::K::list) {
                int64_t i = idx.i;
                if (i < 0 || static_cast<size_t>(i) >= obj.list->items.size()) {
                    panic(e, "list index " + std::to_string(i) + " out of range (len " +
                                 std::to_string(obj.list->items.size()) + ")");
                }
                return obj.list->items[static_cast<size_t>(i)];
            }
            if (obj.k == Value::K::map) {
                for (auto& [k, v] : obj.map->entries) {
                    if (value_eq(k, idx)) return v;
                }
                panic(e, "map key not found: " + display(idx));
            }
            panic(e, "can't index this value");
        }
        case Expr::Kind::list_lit: {
            Value v;
            v.k = Value::K::list;
            v.list = std::make_shared<ListVal>();
            Hint elem = Hint::of(hint.arg(0));
            for (const ExprPtr& el : e->args) {
                v.list->items.push_back(eval(el.get(), env, elem));
            }
            return v;
        }
        case Expr::Kind::init:
            return eval_init(e, env, hint);
        case Expr::Kind::cast: {
            Value v = eval(e->object.get(), env);
            const TypeRef* t = e->type.get();
            std::string tn = t ? (t->resolved.empty() ? t->name : t->resolved) : "";
            if (e->checked) {
                // as? targets are always user classes — fall back to the
                // running package's name for unchecked ASTs
                if (t && t->resolved.empty()) tn = qual(t->name);
                if (v.k == Value::K::instance && class_is(v.inst->cls, tn)) return some(v);
                return none();
            }
            if (tn == "decimal") {
                if (v.k == Value::K::int_) return Value::of_dec(Decimal::from_int(v.i));
                if (v.k == Value::K::float_) {
                    char buf[64];
                    std::snprintf(buf, sizeof buf, "%.17g", v.f);
                    return Value::of_dec(Decimal::parse(buf));
                }
                return v;
            }
            if (tn == "float" || tn == "f64" || tn == "f32") {
                if (v.k == Value::K::int_) return Value::of_float(static_cast<double>(v.i));
                if (v.k == Value::K::decimal_) return Value::of_float(v.dec.to_double());
                return v;
            }
            if (tn == "int" || tn == "i64") {
                if (v.k == Value::K::float_) return Value::of_int(static_cast<int64_t>(v.f));
                if (v.k == Value::K::decimal_) return Value::of_int(v.dec.to_int());
                return v;
            }
            if (tn == "i8") { v.i = static_cast<int8_t>(v.i); return v; }
            if (tn == "i16") { v.i = static_cast<int16_t>(v.i); return v; }
            if (tn == "i32") { v.i = static_cast<int32_t>(v.i); return v; }
            if (tn == "u8" || tn == "byte") { v.i = static_cast<uint8_t>(v.i); return v; }
            if (tn == "u16") { v.i = static_cast<uint16_t>(v.i); return v; }
            if (tn == "u32") { v.i = static_cast<uint32_t>(v.i); return v; }
            if (tn == "u64") return v;
            return v; // upcasts are free
        }
        case Expr::Kind::try_: {
            Value v = eval(e->object.get(), env);
            if (v.k == Value::K::enum_v) {
                if (v.en->variant == "ok" || v.en->variant == "some") {
                    return v.en->payload.empty() ? Value::unit() : v.en->payload[0];
                }
                ReturnSignal r;
                r.v = v;
                throw r;
            }
            panic(e, "? on a non-Result value");
        }
        case Expr::Kind::closure: {
            Value v;
            v.k = Value::K::closure;
            v.clo = std::make_shared<ClosureVal>();
            v.clo->node = e;
            v.clo->captured = env;
            v.clo->pkg = g_pkg;
            return v;
        }
        case Expr::Kind::if_expr: {
            Value c = eval(e->cond.get(), env);
            return c.b ? eval(e->then_e.get(), env, hint)
                       : eval(e->else_e.get(), env, hint);
        }
        case Expr::Kind::match_expr:
            return eval_match(e, env, hint);
    }
    return Value::unit();
}

Value Interp::eval_binary(const Expr* e, std::shared_ptr<Env>& env) {
    // short-circuit first
    if (e->op == TokenKind::andand) {
        Value l = eval(e->lhs.get(), env);
        if (!l.b) return Value::of_bool(false);
        return Value::of_bool(eval(e->rhs.get(), env).b);
    }
    if (e->op == TokenKind::oror) {
        Value l = eval(e->lhs.get(), env);
        if (l.b) return Value::of_bool(true);
        return Value::of_bool(eval(e->rhs.get(), env).b);
    }

    Value l = eval(e->lhs.get(), env);
    Hint rh;
    if (l.k == Value::K::decimal_) rh = Hint::decimal();
    if (l.k == Value::K::float_) rh = Hint::floating();
    Value r = eval(e->rhs.get(), env, rh);
    if (l.k != r.k) {
        // literal on the left adapting to the right (checker guaranteed validity)
        Hint lh;
        if (r.k == Value::K::decimal_) lh = Hint::decimal();
        if (r.k == Value::K::float_) lh = Hint::floating();
        l = eval(e->lhs.get(), env, lh);
    }

    TokenKind op = e->op;
    auto cmp_result = [&](int c) -> Value {
        switch (op) {
            case TokenKind::eq: return Value::of_bool(c == 0);
            case TokenKind::neq: return Value::of_bool(c != 0);
            case TokenKind::lt: return Value::of_bool(c < 0);
            case TokenKind::le: return Value::of_bool(c <= 0);
            case TokenKind::gt: return Value::of_bool(c > 0);
            case TokenKind::ge: return Value::of_bool(c >= 0);
            default: return Value::of_bool(false);
        }
    };

    switch (l.k) {
        case Value::K::int_: {
            int64_t a = l.i, b = r.i;
            switch (op) {
                case TokenKind::plus: return Value::of_int(a + b);
                case TokenKind::minus: return Value::of_int(a - b);
                case TokenKind::star: return Value::of_int(a * b);
                case TokenKind::slash:
                    if (b == 0) panic(e, "divide by zero");
                    return Value::of_int(a / b);
                case TokenKind::percent:
                    if (b == 0) panic(e, "modulo by zero");
                    return Value::of_int(a % b);
                case TokenKind::shl: return Value::of_int(a << b);
                case TokenKind::shr: return Value::of_int(a >> b);
                case TokenKind::amp: return Value::of_int(a & b);
                case TokenKind::pipe: return Value::of_int(a | b);
                case TokenKind::caret: return Value::of_int(a ^ b);
                default: return cmp_result(a < b ? -1 : a > b ? 1 : 0);
            }
        }
        case Value::K::float_: {
            double a = l.f, b = r.f;
            switch (op) {
                case TokenKind::plus: return Value::of_float(a + b);
                case TokenKind::minus: return Value::of_float(a - b);
                case TokenKind::star: return Value::of_float(a * b);
                case TokenKind::slash: return Value::of_float(a / b);
                default: return cmp_result(a < b ? -1 : a > b ? 1 : 0);
            }
        }
        case Value::K::decimal_: {
            switch (op) {
                case TokenKind::plus: return Value::of_dec(l.dec.add(r.dec));
                case TokenKind::minus: return Value::of_dec(l.dec.sub(r.dec));
                case TokenKind::star: return Value::of_dec(l.dec.mul(r.dec));
                case TokenKind::slash:
                    if (r.dec.coeff == 0) panic(e, "divide by zero");
                    return Value::of_dec(l.dec.div(r.dec));
                default: return cmp_result(l.dec.cmp(r.dec));
            }
        }
        case Value::K::string_: {
            int c = l.s->compare(*r.s);
            return cmp_result(c < 0 ? -1 : c > 0 ? 1 : 0);
        }
        case Value::K::bool_: {
            switch (op) {
                case TokenKind::eq: return Value::of_bool(l.b == r.b);
                case TokenKind::neq: return Value::of_bool(l.b != r.b);
                default: break;
            }
            return Value::of_bool(false);
        }
        case Value::K::enum_v: {
            bool same = value_eq(l, r);
            return Value::of_bool(op == TokenKind::eq ? same : !same);
        }
        default:
            return Value::of_bool(false);
    }
}

Value Interp::eval_init(const Expr* e, std::shared_ptr<Env>& env, Hint hint) {
    std::string cname = e->resolved.empty() ? e->name : e->resolved;
    bool pinned = !e->resolved.empty();
    if (cname.empty() && hint.tref && hint.tref->kind == TypeRef::Kind::named) {
        if (hint.tref->name == "Map") {
            Value v;
            v.k = Value::K::map;
            v.map = std::make_shared<MapVal>();
            Hint kh = Hint::of(hint.arg(0));
            Hint vh = Hint::of(hint.arg(1));
            for (const InitEntry& en : e->entries) {
                Value key = en.key ? eval(en.key.get(), env, kh)
                                   : Value::of_str(en.name);
                Value val = eval(en.value.get(), env, vh);
                v.map->entries.emplace_back(std::move(key), std::move(val));
            }
            return v;
        }
        if (!hint.tref->resolved.empty()) {
            cname = hint.tref->resolved;
            pinned = true;
        } else {
            cname = hint.tref->name;
        }
    }
    if (const ClassDecl* cls = find_class(cname)) {
        return make_instance(cls, e->entries, env);
    }
    if (!pinned && !cname.empty()) {
        // plain name in a dep package's own code (interpolation segments)
        if (const ClassDecl* cls = find_class(qual(cname))) {
            return make_instance(cls, e->entries, env);
        }
    }
    // map literal with string keys, no hint needed at runtime
    if (cname.empty() || cname == "Map") {
        Value v;
        v.k = Value::K::map;
        v.map = std::make_shared<MapVal>();
        for (const InitEntry& en : e->entries) {
            Value key = en.key ? eval(en.key.get(), env) : Value::of_str(en.name);
            v.map->entries.emplace_back(std::move(key), eval(en.value.get(), env));
        }
        return v;
    }
    panic(e, "can't build '" + cname + "'");
}

// ---- calls ------------------------------------------------------------------

Value Interp::eval_call(const Expr* e, std::shared_ptr<Env>& env, Hint hint) {
    const Expr* callee = e->callee.get();

    auto eval_args_hinted = [&](const std::vector<Param>& params) {
        std::vector<Value> out;
        for (size_t i = 0; i < e->args.size(); i++) {
            Hint h = i < params.size() ? Hint::of(params[i].type.get()) : Hint();
            out.push_back(eval(e->args[i].get(), env, h));
        }
        return out;
    };
    auto eval_args_plain = [&]() {
        std::vector<Value> out;
        for (const ExprPtr& a : e->args) out.push_back(eval(a.get(), env));
        return out;
    };
    auto call_value = [&](Value& f, std::vector<Value> args) -> Value {
        if (f.k == Value::K::closure) return call_closure(*f.clo, std::move(args));
        if (f.k == Value::K::fn_ref)
            return call_fn(f.fnr->decl, nullptr, std::move(args),
                           pkg_of(f.fnr->decl->qualname));
        panic(e, "not callable");
    };

    if (callee->kind == Expr::Kind::ident) {
        std::string name(callee->text);

        if (Value* v = env->find(name)) {
            Value fv = *v; // copy before arg eval — env may grow underneath
            std::vector<Value> args;
            if (fv.k == Value::K::closure) {
                for (size_t i = 0; i < e->args.size(); i++) {
                    Hint h = i < fv.clo->node->params.size()
                                 ? Hint::of(fv.clo->node->params[i].type.get())
                                 : Hint();
                    args.push_back(eval(e->args[i].get(), env, h));
                }
            } else {
                args = eval_args_plain();
            }
            return call_value(fv, std::move(args));
        }

        if (name == "some") {
            return some(eval(e->args[0].get(), env, Hint::of(hint.arg(0))));
        }
        if (name == "ok" || name == "err") {
            Value x;
            x.k = Value::K::enum_v;
            x.en = std::make_shared<EnumVal>();
            x.en->enum_name = "Result";
            x.en->variant = name;
            if (name == "ok") {
                x.en->payload.push_back(eval(e->args[0].get(), env, Hint::of(hint.arg(0))));
            } else {
                Value arg = eval(e->args[0].get(), env);
                if (arg.k == Value::K::string_) arg = make_err(*arg.s);
                x.en->payload.push_back(std::move(arg));
            }
            return x;
        }

        auto fit = fns_.find(callee->resolved.empty() ? qual(name) : callee->resolved);
        if (fit != fns_.end()) {
            return call_fn(fit->second, nullptr, eval_args_hinted(fit->second->params),
                           pkg_of(fit->second->qualname));
        }
        panic(e, "unknown function '" + name + "'");
    }

    if (callee->kind == Expr::Kind::field) {
        const Expr* obj = callee->object.get();
        const std::string& mname = callee->name;

        auto do_println = [&](bool newline) {
            Value v = eval(e->args[0].get(), env);
            std::string out = display(v);
            if (newline) out += "\n";
            std::fwrite(out.data(), 1, out.size(), stdout);
            return Value::unit();
        };
        auto do_spawn = [&]() {
            Value f = eval(e->args[0].get(), env);
            Value tv;
            tv.k = Value::K::thread;
            tv.thread = std::make_shared<ThreadVal>();
            tv.thread->result = std::make_shared<Value>();
            tv.thread->panic = std::make_shared<std::string>();
            auto result = tv.thread->result;
            auto panic_slot = tv.thread->panic;
            ClosureVal clo = *f.clo;
            tv.thread->th = std::thread([this, clo, result, panic_slot]() {
                try {
                    *result = call_closure(clo, {});
                } catch (const BeansPanic& p) {
                    *panic_slot = p.msg.empty() ? "panic" : p.msg;
                } catch (...) {
                    *panic_slot = "unknown panic";
                }
            });
            return tv;
        };

        // the checker pinned this call: a std builtin or a package function
        if (!callee->resolved.empty()) {
            const std::string& r = callee->resolved;
            if (r == "std.io.println") return do_println(true);
            if (r == "std.io.print") return do_println(false);
            if (r == "std.thread.spawn") return do_spawn();
            auto fit = fns_.find(r);
            if (fit != fns_.end()) {
                return call_fn(fit->second, nullptr, eval_args_hinted(fit->second->params),
                               pkg_of(fit->second->qualname));
            }
        }

        if (obj->kind == Expr::Kind::ident && !env->find(std::string(obj->text))) {
            std::string n(obj->text);

            // unannotated package call (string-interpolation segments)
            std::string path = binding_path(n);
            if (!path.empty()) {
                if (path == "std.io" && (mname == "println" || mname == "print")) {
                    return do_println(mname == "println");
                }
                if (path == "std.thread" && mname == "spawn") return do_spawn();
                auto pfx = prefix_by_path_.find(path);
                if (pfx != prefix_by_path_.end()) {
                    auto fit = fns_.find(pfx->second + "." + mname);
                    if (fit != fns_.end()) {
                        return call_fn(fit->second, nullptr,
                                       eval_args_hinted(fit->second->params),
                                       pkg_of(fit->second->qualname));
                    }
                }
                panic(e, "package '" + n + "' has nothing runnable named '" + mname + "'");
            }

            if (const EnumDecl* ed = resolve_enum(obj, n)) {
                Value x;
                x.k = Value::K::enum_v;
                x.en = std::make_shared<EnumVal>();
                x.en->enum_name = ed->qualname;
                x.en->variant = mname;
                const EnumVariant* var = nullptr;
                for (const EnumVariant& v : ed->variants) {
                    if (v.name == mname) var = &v;
                }
                for (size_t i = 0; i < e->args.size(); i++) {
                    Hint h = var && i < var->payload.size()
                                 ? Hint::of(var->payload[i].type.get())
                                 : Hint();
                    x.en->payload.push_back(eval(e->args[i].get(), env, h));
                }
                return x;
            }

            if (n == "Mutex" && mname == "new") {
                Value inner = eval(e->args[0].get(), env, Hint::of(hint.arg(0)));
                Value v;
                v.k = Value::K::mutex;
                v.mutex = std::make_shared<MutexVal>();
                v.mutex->inner = std::make_shared<Value>(std::move(inner));
                return v;
            }
            if (n == "Channel" && mname == "new") {
                Value cap = eval(e->args[0].get(), env);
                Value v;
                v.k = Value::K::channel;
                v.chan = std::make_shared<ChannelVal>();
                v.chan->cap = cap.i > 0 ? static_cast<size_t>(cap.i) : 1;
                return v;
            }
            if (n == "AtomicInt" && mname == "new") {
                Value init = eval(e->args[0].get(), env);
                Value v;
                v.k = Value::K::atomic;
                v.atomic = std::make_shared<AtomicVal>();
                v.atomic->v = init.i;
                return v;
            }

            if (const ClassDecl* cls = resolve_class(obj, n)) {
                const FnDecl* m = nullptr;
                for (const FnDecl& f : cls->methods) {
                    if (f.name == mname && !f.has_self) m = &f;
                }
                if (!m) panic(e, n + " has no static '" + mname + "'");
                return call_fn(m, nullptr, eval_args_hinted(m->params),
                               pkg_of(cls->qualname));
            }
        }

        // `util.Payment.card(...)` / `util.User.new(...)` — the receiver is a
        // field expression the checker resolved to a type
        if (obj->kind == Expr::Kind::field && !obj->resolved.empty()) {
            if (const EnumDecl* ed = resolve_enum(obj, "")) {
                Value x;
                x.k = Value::K::enum_v;
                x.en = std::make_shared<EnumVal>();
                x.en->enum_name = ed->qualname;
                x.en->variant = mname;
                const EnumVariant* var = nullptr;
                for (const EnumVariant& v : ed->variants) {
                    if (v.name == mname) var = &v;
                }
                for (size_t i = 0; i < e->args.size(); i++) {
                    Hint h = var && i < var->payload.size()
                                 ? Hint::of(var->payload[i].type.get())
                                 : Hint();
                    x.en->payload.push_back(eval(e->args[i].get(), env, h));
                }
                return x;
            }
            if (const ClassDecl* cls = find_class(obj->resolved)) {
                const FnDecl* m = nullptr;
                for (const FnDecl& f : cls->methods) {
                    if (f.name == mname && !f.has_self) m = &f;
                }
                if (!m) panic(e, obj->resolved + " has no static '" + mname + "'");
                return call_fn(m, nullptr, eval_args_hinted(m->params),
                               pkg_of(cls->qualname));
            }
        }

        // instance call
        Value recv = eval(obj, env);
        if (recv.k == Value::K::instance && recv.inst->cls) {
            const ClassDecl* owner = recv.inst->cls;
            if (const FnDecl* m = find_method(recv.inst->cls, mname, &owner)) {
                return call_fn(m, &recv, eval_args_hinted(m->params),
                               pkg_of(owner->qualname));
            }
        }
        if (recv.k == Value::K::enum_v) {
            auto eit = enums_.find(recv.en->enum_name);
            if (eit != enums_.end()) {
                for (const FnDecl& m : eit->second->methods) {
                    if (m.name == mname) {
                        return call_fn(&m, &recv, eval_args_hinted(m.params),
                                       pkg_of(eit->second->qualname));
                    }
                }
            }
        }
        std::vector<Value> args = eval_args_plain();
        return eval_builtin_method(e, recv, mname, args);
    }

    Value f = eval(callee, env);
    return call_value(f, eval_args_plain());
}

// ---- builtin methods --------------------------------------------------------

Value Interp::eval_builtin_method(const Expr* e, Value& recv, const std::string& name,
                                  std::vector<Value>& args) {
    switch (recv.k) {
        case Value::K::int_:
            if (name == "abs") return Value::of_int(recv.i < 0 ? -recv.i : recv.i);
            break;
        case Value::K::float_:
            if (name == "abs") return Value::of_float(std::fabs(recv.f));
            if (name == "round") return Value::of_int(std::llround(recv.f));
            break;
        case Value::K::decimal_:
            if (name == "abs") return Value::of_dec(recv.dec.abs());
            if (name == "round")
                return Value::of_dec(recv.dec.round_to(static_cast<int32_t>(args[0].i)));
            break;
        case Value::K::string_: {
            const std::string& s = *recv.s;
            if (name == "len") return Value::of_int(static_cast<int64_t>(s.size()));
            if (name == "to_int") {
                const char* p = s.c_str();
                char* end = nullptr;
                long long v = std::strtoll(p, &end, 10);
                Value x;
                x.k = Value::K::enum_v;
                x.en = std::make_shared<EnumVal>();
                x.en->enum_name = "Result";
                if (end == p || *end != '\0') {
                    x.en->variant = "err";
                    x.en->payload.push_back(make_err("can't read '" + s + "' as int"));
                } else {
                    x.en->variant = "ok";
                    x.en->payload.push_back(Value::of_int(v));
                }
                return x;
            }
            if (name == "last") {
                int64_t n = args[0].i;
                if (n < 0) n = 0;
                size_t take = static_cast<size_t>(n) > s.size() ? s.size()
                                                                : static_cast<size_t>(n);
                return Value::of_str(s.substr(s.size() - take));
            }
            if (name == "contains")
                return Value::of_bool(s.find(*args[0].s) != std::string::npos);
            break;
        }
        case Value::K::list: {
            auto& items = recv.list->items;
            if (name == "push") { items.push_back(args[0]); return Value::unit(); }
            if (name == "pop") {
                if (items.empty()) return none();
                Value v = items.back();
                items.pop_back();
                return some(std::move(v));
            }
            if (name == "get") {
                int64_t i = args[0].i;
                if (i < 0 || static_cast<size_t>(i) >= items.size()) return none();
                return some(items[static_cast<size_t>(i)]);
            }
            if (name == "len") return Value::of_int(static_cast<int64_t>(items.size()));
            if (name == "contains") {
                for (const Value& v : items) {
                    if (value_eq(v, args[0])) return Value::of_bool(true);
                }
                return Value::of_bool(false);
            }
            if (name == "max") {
                if (items.empty()) return none();
                Value best = items[0];
                for (const Value& v : items) {
                    bool bigger = false;
                    if (v.k == Value::K::int_) bigger = v.i > best.i;
                    else if (v.k == Value::K::float_) bigger = v.f > best.f;
                    else if (v.k == Value::K::decimal_) bigger = v.dec.cmp(best.dec) > 0;
                    else if (v.k == Value::K::string_) bigger = *v.s > *best.s;
                    if (bigger) best = v;
                }
                return some(std::move(best));
            }
            if (name == "join") {
                std::string out;
                for (size_t i = 0; i < items.size(); i++) {
                    if (i) out += *args[0].s;
                    out += display(items[i]);
                }
                return Value::of_str(std::move(out));
            }
            break;
        }
        case Value::K::map: {
            auto& entries = recv.map->entries;
            if (name == "get") {
                for (auto& [k, v] : entries) {
                    if (value_eq(k, args[0])) return some(v);
                }
                return none();
            }
            if (name == "set") {
                for (auto& [k, v] : entries) {
                    if (value_eq(k, args[0])) { v = args[1]; return Value::unit(); }
                }
                entries.emplace_back(args[0], args[1]);
                return Value::unit();
            }
            if (name == "len") return Value::of_int(static_cast<int64_t>(entries.size()));
            if (name == "contains") {
                for (auto& [k, v] : entries) {
                    if (value_eq(k, args[0])) return Value::of_bool(true);
                }
                return Value::of_bool(false);
            }
            break;
        }
        case Value::K::enum_v: {
            const std::string& variant = recv.en->variant;
            bool has = variant == "some" || variant == "ok";
            if (name == "or") {
                if (has && !recv.en->payload.empty()) return recv.en->payload[0];
                return args[0];
            }
            if (name == "expect") {
                if (has && !recv.en->payload.empty()) return recv.en->payload[0];
                panic(e, *args[0].s);
            }
            if (name == "is_some") return Value::of_bool(variant == "some");
            if (name == "is_none") return Value::of_bool(variant == "none");
            if (name == "is_ok") return Value::of_bool(variant == "ok");
            break;
        }
        case Value::K::thread: {
            if (name == "join") {
                if (recv.thread->joined) panic(e, "thread already joined");
                recv.thread->joined = true;
                if (recv.thread->th.joinable()) recv.thread->th.join();
                if (!recv.thread->panic->empty()) {
                    panic(e, "thread panicked: " + *recv.thread->panic);
                }
                return *recv.thread->result;
            }
            break;
        }
        case Value::K::mutex: {
            if (name == "with") {
                std::lock_guard<std::mutex> lock(recv.mutex->m);
                Value f = args[0];
                if (f.k == Value::K::closure) {
                    call_closure(*f.clo, {*recv.mutex->inner});
                }
                return Value::unit();
            }
            break;
        }
        case Value::K::channel: {
            ChannelVal& ch = *recv.chan;
            if (name == "send") {
                std::unique_lock<std::mutex> lock(ch.m);
                ch.cv_send.wait(lock, [&] { return ch.q.size() < ch.cap || ch.closed; });
                if (ch.closed) panic(e, "send on a closed channel");
                ch.q.push_back(args[0]);
                ch.cv_recv.notify_one();
                return Value::unit();
            }
            if (name == "recv") {
                std::unique_lock<std::mutex> lock(ch.m);
                ch.cv_recv.wait(lock, [&] { return !ch.q.empty() || ch.closed; });
                if (ch.q.empty()) return none();
                Value v = ch.q.front();
                ch.q.pop_front();
                ch.cv_send.notify_one();
                return some(std::move(v));
            }
            if (name == "close") {
                std::lock_guard<std::mutex> lock(ch.m);
                ch.closed = true;
                ch.cv_send.notify_all();
                ch.cv_recv.notify_all();
                return Value::unit();
            }
            break;
        }
        case Value::K::atomic: {
            if (name == "add") return Value::of_int(recv.atomic->v.fetch_add(args[0].i) + args[0].i);
            if (name == "get") return Value::of_int(recv.atomic->v.load());
            if (name == "set") { recv.atomic->v.store(args[0].i); return Value::unit(); }
            break;
        }
        default:
            break;
    }
    panic(e, "no method '" + name + "' here");
}

// ---- match ------------------------------------------------------------------

bool Interp::match_pattern(const Pattern* p, const Value& v, Env& bind_env,
                           std::shared_ptr<Env>& outer) {
    switch (p->kind) {
        case Pattern::Kind::wildcard:
            return true;
        case Pattern::Kind::alt:
            for (const PatPtr& a : p->alts) {
                if (match_pattern(a.get(), v, bind_env, outer)) return true;
            }
            return false;
        case Pattern::Kind::literal: {
            Hint h;
            if (v.k == Value::K::decimal_) h = Hint::decimal();
            if (v.k == Value::K::float_) h = Hint::floating();
            Value lit = eval(p->lit.get(), outer, h);
            return value_eq(lit, v);
        }
        case Pattern::Kind::range: {
            Value lo = eval(p->lit.get(), outer);
            Value hi = eval(p->lit2.get(), outer);
            if (v.k != Value::K::int_) return false;
            return v.i >= lo.i && (p->inclusive ? v.i <= hi.i : v.i < hi.i);
        }
        case Pattern::Kind::name: {
            if (v.k != Value::K::enum_v || v.en->variant != p->name) return false;
            for (size_t i = 0; i < p->bindings.size() && i < v.en->payload.size(); i++) {
                bind_env.declare(p->bindings[i].name, v.en->payload[i]);
            }
            return true;
        }
    }
    return false;
}

Value Interp::eval_match(const Expr* e, std::shared_ptr<Env>& env, Hint hint) {
    Value subj = eval(e->subject.get(), env);
    for (const MatchArm& arm : e->arms) {
        auto arm_env = std::make_shared<Env>();
        arm_env->parent = env;
        if (match_pattern(arm.pat.get(), subj, *arm_env, env)) {
            if (arm.is_block) {
                exec_block(arm.body, arm_env);
                return Value::unit();
            }
            return eval(arm.value.get(), arm_env, hint);
        }
    }
    panic(e, "no match arm fit the value " + display(subj));
}

// ---- strings ----------------------------------------------------------------

const std::vector<Interp::StrPart>& Interp::string_parts(const Expr* e) {
    std::lock_guard<std::mutex> lock(str_cache_mu_);
    auto it = str_cache_.find(e);
    if (it != str_cache_.end()) return it->second;

    std::vector<StrPart> parts;
    std::string_view raw = e->text;
    std::string_view body = raw.size() >= 2 ? raw.substr(1, raw.size() - 2)
                                            : std::string_view{};
    std::string cur;
    size_t i = 0;
    while (i < body.size()) {
        char c = body[i];
        if (c == '\\' && i + 1 < body.size()) {
            char n = body[i + 1];
            switch (n) {
                case 'n': cur += '\n'; break;
                case 't': cur += '\t'; break;
                case 'r': cur += '\r'; break;
                case '0': cur += '\0'; break;
                case '\\': cur += '\\'; break;
                case '"': cur += '"'; break;
                case '{': cur += '{'; break;
                case '}': cur += '}'; break;
                default: cur += n; break;
            }
            i += 2;
            continue;
        }
        if (c == '{') {
            size_t start = i + 1;
            int depth = 1;
            size_t j = start;
            bool in_str = false;
            while (j < body.size() && depth > 0) {
                char d = body[j];
                if (d == '\\') { j += 2; continue; }
                if (in_str) {
                    if (d == '"') in_str = false;
                } else if (d == '"') {
                    in_str = true;
                } else if (d == '{') {
                    depth += 1;
                } else if (d == '}') {
                    depth -= 1;
                }
                j += 1;
            }
            std::string segment(body.substr(start, j - 1 - start));
            if (!cur.empty()) {
                StrPart p;
                p.text = cur;
                parts.push_back(std::move(p));
                cur.clear();
            }
            StrPart p;
            // the parsed expr holds string_views into the segment text,
            // so the part must own that text for as long as it lives
            p.src = std::make_shared<std::string>(std::move(segment));
            Lexer lx(*p.src);
            Parser ps(lx.scan_all());
            p.expr = ps.parse_standalone_expr();
            parts.push_back(std::move(p));
            i = j;
            continue;
        }
        cur += c;
        i += 1;
    }
    if (!cur.empty()) {
        StrPart p;
        p.text = cur;
        parts.push_back(std::move(p));
    }
    return str_cache_.emplace(e, std::move(parts)).first->second;
}

Value Interp::eval_string(const Expr* e, std::shared_ptr<Env>& env) {
    // fast path: no braces at all
    std::string_view raw = e->text;
    bool has_brace = false;
    for (char c : raw) has_brace |= c == '{';
    if (!has_brace) {
        const std::vector<StrPart>& parts = string_parts(e);
        return Value::of_str(parts.empty() ? "" : parts[0].text);
    }
    const std::vector<StrPart>& parts = string_parts(e);
    std::string out;
    for (const StrPart& p : parts) {
        if (p.expr) {
            Value v = eval(p.expr.get(), env);
            out += display(v);
        } else {
            out += p.text;
        }
    }
    return Value::of_str(std::move(out));
}

// ---- misc -------------------------------------------------------------------

bool Interp::value_eq(const Value& a, const Value& b) {
    if (a.k != b.k) return false;
    switch (a.k) {
        case Value::K::unit: return true;
        case Value::K::int_: return a.i == b.i;
        case Value::K::float_: return a.f == b.f;
        case Value::K::decimal_: return a.dec.cmp(b.dec) == 0;
        case Value::K::bool_: return a.b == b.b;
        case Value::K::string_: return *a.s == *b.s;
        case Value::K::enum_v: {
            if (a.en->enum_name != b.en->enum_name || a.en->variant != b.en->variant)
                return false;
            if (a.en->payload.size() != b.en->payload.size()) return false;
            for (size_t i = 0; i < a.en->payload.size(); i++) {
                if (!value_eq(a.en->payload[i], b.en->payload[i])) return false;
            }
            return true;
        }
        case Value::K::instance: return a.inst == b.inst;
        case Value::K::list: return a.list == b.list;
        default: return false;
    }
}

std::string Interp::display(const Value& v) {
    switch (v.k) {
        case Value::K::unit: return "()";
        case Value::K::int_: return std::to_string(v.i);
        case Value::K::float_: {
            char buf[64];
            std::snprintf(buf, sizeof buf, "%.10g", v.f);
            return buf;
        }
        case Value::K::decimal_: return v.dec.to_string();
        case Value::K::bool_: return v.b ? "true" : "false";
        case Value::K::string_: return *v.s;
        case Value::K::enum_v: {
            if (v.en->payload.empty()) return v.en->variant;
            std::string out = v.en->variant + "(";
            for (size_t i = 0; i < v.en->payload.size(); i++) {
                if (i) out += ", ";
                out += display(v.en->payload[i]);
            }
            return out + ")";
        }
        case Value::K::instance:
            return v.inst->cls ? v.inst->cls->name : "Error";
        case Value::K::list: {
            std::string out = "[";
            for (size_t i = 0; i < v.list->items.size(); i++) {
                if (i) out += ", ";
                out += display(v.list->items[i]);
            }
            return out + "]";
        }
        case Value::K::map: return "{map}";
        case Value::K::closure: return "{fn}";
        case Value::K::fn_ref: return "{fn}";
        case Value::K::range: return "{range}";
        case Value::K::thread: return "{thread}";
        case Value::K::mutex: return "{mutex}";
        case Value::K::channel: return "{channel}";
        case Value::K::atomic: return "{atomic}";
    }
    return "?";
}

Value Interp::coerce_arg(Value v, const TypeRef*) { return v; }

} // namespace beans
