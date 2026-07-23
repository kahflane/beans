#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint8_t tag;
    uint32_t count;
    float ratio;
    _Bool live;
} BeansTestPacket;

typedef union {
    uint32_t bits;
    float number;
} BeansTestWord;

typedef struct {
    uint32_t code;
    uint8_t* next;
} BeansTestLink;

typedef struct {
    uint16_t small;
    uint8_t bytes[3];
} BeansTestPair;

typedef union {
    uint8_t bytes[16];
    uint64_t word;
} BeansTestAlignedBlock;

typedef struct {
    BeansTestPair pair;
    uint32_t values[2];
    BeansTestAlignedBlock block;
} BeansTestFrame;

static uint8_t beans_test_link_byte = 77;

unsigned long long beans_test_packet_size(void) { return sizeof(BeansTestPacket); }
unsigned long long beans_test_packet_align(void) { return _Alignof(BeansTestPacket); }
unsigned long long beans_test_packet_offset(unsigned long long index) {
    static const size_t offsets[] = {
        offsetof(BeansTestPacket, tag),
        offsetof(BeansTestPacket, count),
        offsetof(BeansTestPacket, ratio),
        offsetof(BeansTestPacket, live),
    };
    return index < 4 ? offsets[index] : (unsigned long long)-1;
}
void beans_test_packet_fill(void* raw) {
    BeansTestPacket* value = raw;
    value->tag = 9;
    value->count = 123;
    value->ratio = 2.5f;
    value->live = 1;
}
BeansTestPacket beans_test_packet_roundtrip(BeansTestPacket value,
                                             uint32_t extra) {
    value.count += extra;
    value.ratio += 0.5f;
    return value;
}

unsigned long long beans_test_word_size(void) { return sizeof(BeansTestWord); }
unsigned long long beans_test_word_align(void) { return _Alignof(BeansTestWord); }
unsigned long long beans_test_word_offset(unsigned long long index) {
    static const size_t offsets[] = {
        offsetof(BeansTestWord, bits),
        offsetof(BeansTestWord, number),
    };
    return index < 2 ? offsets[index] : (unsigned long long)-1;
}
void beans_test_word_fill(void* raw) {
    BeansTestWord* value = raw;
    value->number = 3.0f;
}
BeansTestWord beans_test_word_roundtrip(BeansTestWord value) {
    value.bits ^= UINT32_C(0x00800000);
    return value;
}

unsigned long long beans_test_link_size(void) { return sizeof(BeansTestLink); }
unsigned long long beans_test_link_align(void) { return _Alignof(BeansTestLink); }
unsigned long long beans_test_link_offset(unsigned long long index) {
    static const size_t offsets[] = {
        offsetof(BeansTestLink, code),
        offsetof(BeansTestLink, next),
    };
    return index < 2 ? offsets[index] : (unsigned long long)-1;
}
void beans_test_link_fill(void* raw) {
    BeansTestLink* value = raw;
    value->code = 55;
    value->next = &beans_test_link_byte;
}

unsigned long long beans_test_pair_size(void) { return sizeof(BeansTestPair); }
unsigned long long beans_test_pair_align(void) { return _Alignof(BeansTestPair); }
unsigned long long beans_test_pair_offset(unsigned long long index) {
    static const size_t offsets[] = {
        offsetof(BeansTestPair, small),
        offsetof(BeansTestPair, bytes),
    };
    return index < 2 ? offsets[index] : (unsigned long long)-1;
}
unsigned long long beans_test_block_size(void) { return sizeof(BeansTestAlignedBlock); }
unsigned long long beans_test_block_align(void) { return _Alignof(BeansTestAlignedBlock); }
unsigned long long beans_test_block_offset(unsigned long long index) {
    static const size_t offsets[] = {
        offsetof(BeansTestAlignedBlock, bytes),
        offsetof(BeansTestAlignedBlock, word),
    };
    return index < 2 ? offsets[index] : (unsigned long long)-1;
}

unsigned long long beans_test_frame_size(void) { return sizeof(BeansTestFrame); }
unsigned long long beans_test_frame_align(void) { return _Alignof(BeansTestFrame); }
unsigned long long beans_test_frame_offset(unsigned long long index) {
    static const size_t offsets[] = {
        offsetof(BeansTestFrame, pair),
        offsetof(BeansTestFrame, values),
        offsetof(BeansTestFrame, block),
    };
    return index < 3 ? offsets[index] : (unsigned long long)-1;
}
void beans_test_frame_fill(void* raw) {
    BeansTestFrame* value = raw;
    value->pair.small = 513;
    value->pair.bytes[0] = 4;
    value->pair.bytes[1] = 5;
    value->pair.bytes[2] = 6;
    value->values[0] = 1000;
    value->values[1] = 2000;
    value->block.word = UINT64_C(0x0102030405060708);
}
BeansTestFrame beans_test_frame_roundtrip(BeansTestFrame value) {
    value.pair.small += 1;
    value.values[1] += 3;
    value.block.word += 4;
    return value;
}

double beans_test_mixed_float(float first, double second, float third,
                              uint64_t whole) {
    return (double)first + second + (double)third + (double)whole;
}
