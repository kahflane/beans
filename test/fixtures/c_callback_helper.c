#include <stdint.h>

typedef struct {
    int32_t x;
    int32_t y;
} BeansTestPoint;

int32_t beans_test_apply_twice(int32_t value,
                               int32_t (*callback)(int32_t, int32_t)) {
    int32_t first = callback(value, 2);
    return callback(first, 3);
}

int32_t beans_test_apply_two(int32_t value,
                             int32_t (*first)(int32_t),
                             int32_t (*second)(int32_t)) {
    return first(value) + second(value);
}

float beans_test_apply_f32(float value, float (*callback)(float, float)) {
    return callback(value, 0.5f);
}

uint32_t beans_test_repeat(void (*callback)(void), uint32_t count) {
    for (uint32_t i = 0; i < count; i++) callback();
    return count;
}

int32_t beans_test_call_once(int32_t (*callback)(void)) {
    return callback();
}

BeansTestPoint beans_test_map_point(
    BeansTestPoint value,
    BeansTestPoint (*callback)(BeansTestPoint, int32_t),
    int32_t amount) {
    return callback(value, amount);
}
