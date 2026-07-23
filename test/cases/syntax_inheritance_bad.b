interface Shape {}
class Base {}
class WrongBase extends Shape {}
class WrongInterface implements Base {}
interface WrongExtends extends Base {}
