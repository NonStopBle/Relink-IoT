// Public types shared by the source build (RelinkIoT.h) and the closed
// build's public header (arduino/closed/RelinkIoT.h): version, data type tags,
// V0..V255, callbacks, RelinkParam and RELINK_WRITE.
#pragma once

#include <Arduino.h>
#include <functional>

// ── Version ───────────────────────────────────────────────────────────────────
#define RELINK_VERSION "1.0.2"

// ── Protocol data type tags ───────────────────────────────────────────────────
#define RELINK_TYPE_INT16  0
#define RELINK_TYPE_INT32  1
#define RELINK_TYPE_FLOAT  2
#define RELINK_TYPE_STRING 3
#define RELINK_TYPE_BYTES  4   // raw bytes, framed like a string ([len][bytes])
// Types whose value carries its own length byte on the wire.
#define RELINK_TYPE_SIZED(t) ((t) == RELINK_TYPE_STRING || (t) == RELINK_TYPE_BYTES)

// ── VPin alias macros ─────────────────────────────────────────────────────────
#define V0   0
#define V1   1
#define V2   2
#define V3   3
#define V4   4
#define V5   5
#define V6   6
#define V7   7
#define V8   8
#define V9   9
#define V10  10
#define V11  11
#define V12  12
#define V13  13
#define V14  14
#define V15  15
#define V16  16
#define V17  17
#define V18  18
#define V19  19
#define V20  20
#define V21  21
#define V22  22
#define V23  23
#define V24  24
#define V25  25
#define V26  26
#define V27  27
#define V28  28
#define V29  29
#define V30  30
#define V31  31
#define V32  32
#define V33  33
#define V34  34
#define V35  35
#define V36  36
#define V37  37
#define V38  38
#define V39  39
#define V40  40
#define V41  41
#define V42  42
#define V43  43
#define V44  44
#define V45  45
#define V46  46
#define V47  47
#define V48  48
#define V49  49
#define V50  50
#define V51  51
#define V52  52
#define V53  53
#define V54  54
#define V55  55
#define V56  56
#define V57  57
#define V58  58
#define V59  59
#define V60  60
#define V61  61
#define V62  62
#define V63  63
#define V64  64
#define V65  65
#define V66  66
#define V67  67
#define V68  68
#define V69  69
#define V70  70
#define V71  71
#define V72  72
#define V73  73
#define V74  74
#define V75  75
#define V76  76
#define V77  77
#define V78  78
#define V79  79
#define V80  80
#define V81  81
#define V82  82
#define V83  83
#define V84  84
#define V85  85
#define V86  86
#define V87  87
#define V88  88
#define V89  89
#define V90  90
#define V91  91
#define V92  92
#define V93  93
#define V94  94
#define V95  95
#define V96  96
#define V97  97
#define V98  98
#define V99  99
#define V100 100
#define V101 101
#define V102 102
#define V103 103
#define V104 104
#define V105 105
#define V106 106
#define V107 107
#define V108 108
#define V109 109
#define V110 110
#define V111 111
#define V112 112
#define V113 113
#define V114 114
#define V115 115
#define V116 116
#define V117 117
#define V118 118
#define V119 119
#define V120 120
#define V121 121
#define V122 122
#define V123 123
#define V124 124
#define V125 125
#define V126 126
#define V127 127
#define V128 128
#define V129 129
#define V130 130
#define V131 131
#define V132 132
#define V133 133
#define V134 134
#define V135 135
#define V136 136
#define V137 137
#define V138 138
#define V139 139
#define V140 140
#define V141 141
#define V142 142
#define V143 143
#define V144 144
#define V145 145
#define V146 146
#define V147 147
#define V148 148
#define V149 149
#define V150 150
#define V151 151
#define V152 152
#define V153 153
#define V154 154
#define V155 155
#define V156 156
#define V157 157
#define V158 158
#define V159 159
#define V160 160
#define V161 161
#define V162 162
#define V163 163
#define V164 164
#define V165 165
#define V166 166
#define V167 167
#define V168 168
#define V169 169
#define V170 170
#define V171 171
#define V172 172
#define V173 173
#define V174 174
#define V175 175
#define V176 176
#define V177 177
#define V178 178
#define V179 179
#define V180 180
#define V181 181
#define V182 182
#define V183 183
#define V184 184
#define V185 185
#define V186 186
#define V187 187
#define V188 188
#define V189 189
#define V190 190
#define V191 191
#define V192 192
#define V193 193
#define V194 194
#define V195 195
#define V196 196
#define V197 197
#define V198 198
#define V199 199
#define V200 200
#define V201 201
#define V202 202
#define V203 203
#define V204 204
#define V205 205
#define V206 206
#define V207 207
#define V208 208
#define V209 209
#define V210 210
#define V211 211
#define V212 212
#define V213 213
#define V214 214
#define V215 215
#define V216 216
#define V217 217
#define V218 218
#define V219 219
#define V220 220
#define V221 221
#define V222 222
#define V223 223
#define V224 224
#define V225 225
#define V226 226
#define V227 227
#define V228 228
#define V229 229
#define V230 230
#define V231 231
#define V232 232
#define V233 233
#define V234 234
#define V235 235
#define V236 236
#define V237 237
#define V238 238
#define V239 239
#define V240 240
#define V241 241
#define V242 242
#define V243 243
#define V244 244
#define V245 245
#define V246 246
#define V247 247
#define V248 248
#define V249 249
#define V250 250
#define V251 251
#define V252 252
#define V253 253
#define V254 254
#define V255 255

// ── Callback types ────────────────────────────────────────────────────────────
typedef std::function<void()>                     RelinkCallback;
typedef std::function<void(int16_t)>              RelinkCallbackInt16;
typedef std::function<void(int32_t)>              RelinkCallbackInt32;
typedef std::function<void(float)>                RelinkCallbackFloat;
typedef std::function<void(const char*, uint8_t)> RelinkCallbackString;
typedef std::function<void(const uint8_t*, uint8_t)> RelinkCallbackBytes;   // ByteArray datastream

// ── RelinkParam — unified value wrapper ──────────────────────────────────────
class RelinkParam {
public:
    RelinkParam(int16_t v)                : _t(0) { _u.i16 = v; }
    RelinkParam(int32_t v)                : _t(1) { _u.i32 = v; }
    RelinkParam(float v)                  : _t(2) { _u.f   = v; }
    RelinkParam(const char* s, uint8_t l) : _t(3), _s(s), _n(l) {}
    RelinkParam(const uint8_t* b, uint8_t l) : _t(4), _s((const char*)b), _n(l) {}   // ByteArray

    // Widgets like Control Slider always send FLOAT regardless of the pin's
    // configured data type, so a RELINK_WRITE(Vx) handler calling asInt()
    // must correctly convert a float-tagged param, not just reinterpret its
    // raw bits as an int32 (which is what happened before -- it only
    // "worked" at 0.0f because the all-zero bit pattern happens to also
    // read as integer 0).
    int16_t     asInt16()  const { return _t == 2 ? (int16_t)_u.f : _u.i16; }
    int32_t     asInt32()  const { return _t == 2 ? (int32_t)_u.f : _u.i32; }
    int         asInt()    const {
        if (_t == 2) return (int)_u.f;
        return (int)(_t == 0 ? _u.i16 : _u.i32);
    }
    float       asFloat()  const { return _t == 2 ? _u.f : (_t == 0 ? (float)_u.i16 : (float)_u.i32); }
    const char* c_str()    const { return _s ? _s : ""; }
    uint8_t     length()   const { return _n; }
    String      asString() const { return _s ? String(_s).substring(0, _n) : String(); }
    // ByteArray value (length() bytes), e.g. a packed sensor record.
    const uint8_t* asBytes() const { return (const uint8_t*)_s; }
    bool        isBytes()  const { return _t == 4; }
    bool        isString() const { return _t == 3; }

private:
    uint8_t _t = 0;
    union { int16_t i16; int32_t i32; float f; } _u{};
    const char* _s = nullptr;
    uint8_t     _n = 0;
};

void relinkRegisterMacroHandler(uint8_t pin, void (*handler)(RelinkParam));

// ── Blynk-style macros ────────────────────────────────────────────────────────
// Use __attribute__((constructor)) — supported by GCC (avr-gcc, xtensa-gcc)
#define RELINK_WRITE(vPin) \
    void relinkHandler##vPin(RelinkParam param); \
    void __attribute__((constructor)) _relinkAutoReg##vPin() { \
        relinkRegisterMacroHandler(vPin, relinkHandler##vPin); \
    } \
    void relinkHandler##vPin(RelinkParam param)

#define RELINK_READ(vPin) RELINK_WRITE(vPin)
