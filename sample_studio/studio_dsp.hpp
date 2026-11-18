// Sample Studio's offline processing: the render chain the preview and the save share.
//
// A pull pipeline, one output frame at a time, Q31 words per channel:
//   source (markers, reverse, gain on the 16-bit samples)
//   -> time (beat: WSOLA, pitch kept; then a resampler for tape speed or beat pitch)
//   -> color (drive, bits, rate, the filter sweep, comp)
//   -> DC
// The writer (editor.cpp) adds the fades and channels, which need the
// output's length. Every stage is integer arithmetic in 32 bits (products split
// or through the Blackfin's (T) multiply), so its reference,
// tests/studio_ref.py, makes the same samples. State lives in the
// pipeline object, so a render can stop after any frame and go on in the next
// file-worker job.
#pragma once
#include "bake.hpp"

namespace opxy { namespace studio_dsp {
// Internal linkage: the studio and its player both compile this chain, and the SDK build makes
// an out-of-line inline member strong (Pipeline::color, when the compiler doesn't inline it).
namespace {
using namespace opxy::sample_bake;

// 2^(semitones / 12) as Q16, -24..+24: the pitch control and tape speeds.
const u32 kSemitoneQ16[49] = {
    16384u, 17358u, 18390u, 19484u, 20643u, 21870u, 23170u, 24548u, 26008u,
    27554u, 29193u, 30929u, 32768u, 34716u, 36781u, 38968u, 41285u, 43740u,
    46341u, 49097u, 52016u, 55109u, 58386u, 61858u, 65536u, 69433u, 73562u,
    77936u, 82570u, 87480u, 92682u, 98193u, 104032u, 110218u, 116772u, 123715u,
    131072u, 138866u, 147123u, 155872u, 165140u, 174960u, 185364u, 196386u, 208064u,
    220436u, 233544u, 247431u, 262144u};
// The process page's filter sweep: 48 cutoffs from 50 Hz to 12 kHz, log spaced, as
// f = 2 sin(pi fc / 88200) in Q31 for a state-variable filter run twice a frame
// at 44.1 kHz (other rates move the cutoff with them).
const u32 kCutoffCount = 48;
const u32 kCutoffHz[48] = {
    50u, 56u, 63u, 71u, 80u, 90u, 101u, 113u, 127u, 143u, 160u, 180u,
    203u, 228u, 256u, 287u, 323u, 363u, 408u, 458u, 515u, 579u, 650u, 731u,
    821u, 923u, 1037u, 1165u, 1309u, 1471u, 1653u, 1857u, 2087u, 2345u, 2635u, 2961u,
    3327u, 3739u, 4201u, 4721u, 5305u, 5961u, 6698u, 7527u, 8458u, 9504u, 10679u, 12000u};
const u32 kCutoffQ31[48] = {
    7649110u, 8567002u, 9637876u, 10861731u, 12238566u, 13768382u,
    15451178u, 17286952u, 19428684u, 21876372u, 24477033u, 27536623u,
    31055134u, 34879578u, 39162923u, 43905152u, 49412189u, 55531023u,
    62414576u, 70062780u, 78781459u, 88570465u, 99429603u, 111817416u,
    125580557u, 141177216u, 158606560u, 178173220u, 200181266u, 224933993u,
    252733639u, 283881001u, 318980065u, 358326469u, 402516760u, 452141234u,
    507781633u, 570311366u, 640282932u, 718830460u, 806750075u, 905090959u,
    1014982500u, 1137752278u, 1274439898u, 1426332602u, 1594586574u, 1780397847u};
// The rate control: off, then these sample-and-hold rates in Hz.
const u32 kRateHz[9] = {0u, 32000u, 24000u, 16000u, 12000u, 8000u, 6000u, 4000u, 2000u};
// The bits control: off (16), then these depths.
const u32 kBits[9] = {16u, 12u, 10u, 8u, 6u, 5u, 4u, 3u, 2u};
// The filter sweep (one control, low-pass <- off -> high-pass): a fixed,
// mild resonance (damping 2 x 0.35), the state-variable filter run on the input over 4 (12 dB of
// headroom) with its states held inside +-2^30, so no transient can pin them at the rails (unheld,
// at 12 kHz and resonance 100 both states latch at -1.0 and the sound stops).
const i32 kSweepHalf = 0x2ccccccd;            // 0.35 in Q31
const i32 kSweepLimit = 1 << 30;
inline i32 sweep_clamp(i32 v) { return v < -kSweepLimit ? -kSweepLimit : v > kSweepLimit ? kSweepLimit : v; }
// Bits: rounded, not truncated (a mask adds half a step of DC), on a step that follows
// the sound's peak (a peak envelope, release 2^-12 a frame), like a floating-point converter, so a
// quiet tail keeps its crunch instead of falling under a 4-bit step into silence. Below 2^16 of
// envelope the step stops shrinking.
const u32 kBitsRelease = 12, kBitsFloor = 1u << 16;
// Comp: one knob a, 0..100. Threshold 0 to -30 dB (0.3 a dB down), ratio 1 + 5 a / 100,
// makeup to bring a full-scale peak back to full scale; a peak envelope (attack 2^-3, release
// 2^-13 a frame), the gain recomputed every 16 frames: (T + (env - T) / R) / env to 20 bits.
const u32 kCompThreshold[101] = {
    2147483647u, 2074578464u, 2004148349u, 1936109274u, 1870380066u, 1806882307u, 1745540241u, 1686280684u,
    1629032937u, 1573728701u, 1520301994u, 1468689078u, 1418828375u, 1370660399u, 1324127683u, 1279174712u,
    1235747855u, 1193795301u, 1153267000u, 1114114599u, 1076291388u, 1039752241u, 1004453567u, 970353251u,
    937410611u, 905586345u, 874842484u, 845142351u, 816450511u, 788732734u, 761955951u, 736088216u,
    711098668u, 686957493u, 663635890u, 641106035u, 619341049u, 598314964u, 578002697u, 558380014u,
    539423503u, 521110549u, 503419304u, 486328661u, 469818231u, 453868315u, 438459884u, 423574557u,
    409194573u, 395302777u, 381882595u, 368918016u, 356393574u, 344294325u, 332605836u, 321314160u,
    310405827u, 299867822u, 289687573u, 279852934u, 270352173u, 261173954u, 252307328u, 243741716u,
    235466899u, 227473005u, 219750496u, 212290160u, 205083096u, 198120705u, 191394681u, 184897000u,
    178619909u, 172555920u, 166697798u, 161038555u, 155571437u, 150289924u, 145187713u, 140258718u,
    135497057u, 130897051u, 126453212u, 122160236u, 118013003u, 114006566u, 110136143u, 106397118u,
    102785029u, 99295568u, 95924570u, 92668016u, 89522018u, 86482824u, 83546808u, 80710467u,
    77970418u, 75323391u, 72766228u, 70295878u, 67909395u};
const u32 kCompInverse[101] = {
    65536u, 62415u, 59578u, 56987u, 54613u, 52428u, 50412u, 48545u,
    46811u, 45197u, 43690u, 42281u, 40960u, 39718u, 38550u, 37449u,
    36408u, 35424u, 34492u, 33608u, 32768u, 31968u, 31207u, 30481u,
    29789u, 29127u, 28493u, 27887u, 27306u, 26749u, 26214u, 25700u,
    25206u, 24730u, 24272u, 23831u, 23405u, 22995u, 22598u, 22215u,
    21845u, 21487u, 21140u, 20805u, 20480u, 20164u, 19859u, 19562u,
    19275u, 18995u, 18724u, 18460u, 18204u, 17955u, 17712u, 17476u,
    17246u, 17022u, 16804u, 16591u, 16384u, 16181u, 15984u, 15791u,
    15603u, 15420u, 15240u, 15065u, 14894u, 14727u, 14563u, 14403u,
    14246u, 14093u, 13943u, 13797u, 13653u, 13512u, 13374u, 13239u,
    13107u, 12977u, 12850u, 12725u, 12603u, 12483u, 12365u, 12249u,
    12136u, 12024u, 11915u, 11808u, 11702u, 11599u, 11497u, 11397u,
    11299u, 11202u, 11107u, 11014u, 10922u};
const u32 kCompMakeup[101] = {
    268435456u, 268870151u, 270074260u, 271926940u, 274335514u, 277229997u, 280553644u, 284263310u,
    288323867u, 292705900u, 297387423u, 302347264u, 307570744u, 313046677u, 318760045u, 324702713u,
    330870411u, 337251593u, 343841732u, 350635797u, 357630880u, 364826449u, 372212060u, 379790120u,
    387551717u, 395502907u, 403642449u, 411958648u, 420456900u, 429132359u, 437987309u, 447018439u,
    456222761u, 465606079u, 475158908u, 484879829u, 494778040u, 504834416u, 515070452u, 525466907u,
    536025471u, 546749124u, 557642301u, 568682630u, 579888663u, 591267952u, 602780815u, 614466505u,
    626282622u, 638273660u, 650394589u, 662675128u, 675086544u, 687640609u, 700352068u, 713189738u,
    726168796u, 739281124u, 752517846u, 765898304u, 779385400u, 793030729u, 806764431u, 820641635u,
    834621353u, 848693275u, 862922351u, 877226479u, 891634173u, 906136767u, 920768898u, 935479937u,
    950306584u, 965194054u, 980181201u, 995208906u, 1010372112u, 1025612529u, 1040921784u, 1056291048u,
    1071711028u, 1087235048u, 1102793703u, 1118443436u, 1134110162u, 1149853566u, 1165668004u, 1181547551u,
    1197408397u, 1213397030u, 1229349184u, 1245335277u, 1261434516u, 1277468143u, 1293603869u, 1309746771u,
    1325888291u, 1342118381u, 1358334149u, 1374526246u, 1390791497u};
const u32 kCompAttack = 3, kCompRelease = 13, kCompEvery = 16;
// num / den to 20 bits (num < den < 2^31), the drive's long division unrolled without branches.
inline u32 frac20(u32 r, u32 dn) {
    u32 q = 0;
#define OPXY_FRAC_STEP                          \
    {                                           \
        r <<= 1;                                \
        const u32 bit = r >= dn ? 1u : 0u;      \
        r -= dn & (0u - bit);                   \
        q = (q << 1) | bit;                     \
    }
    OPXY_FRAC_STEP OPXY_FRAC_STEP OPXY_FRAC_STEP OPXY_FRAC_STEP OPXY_FRAC_STEP
    OPXY_FRAC_STEP OPXY_FRAC_STEP OPXY_FRAC_STEP OPXY_FRAC_STEP OPXY_FRAC_STEP
    OPXY_FRAC_STEP OPXY_FRAC_STEP OPXY_FRAC_STEP OPXY_FRAC_STEP OPXY_FRAC_STEP
    OPXY_FRAC_STEP OPXY_FRAC_STEP OPXY_FRAC_STEP OPXY_FRAC_STEP OPXY_FRAC_STEP
#undef OPXY_FRAC_STEP
    return q;
}
inline u32 magnitude(i32 x) { return x < 0 ? 0u - static_cast<u32>(x) : static_cast<u32>(x); }
// The index of v's top set bit (v > 0), by halves: no runtime call.
inline u32 top_bit(u32 v) {
    u32 n = 0;
    if (v >= 1u << 16) { v >>= 16; n += 16; }
    if (v >= 1u << 8) { v >>= 8; n += 8; }
    if (v >= 1u << 4) { v >>= 4; n += 4; }
    if (v >= 1u << 2) { v >>= 2; n += 2; }
    if (v >= 1u << 1) n += 1;
    return n;
}
// x times a Q4.28 gain, saturated.
inline i32 mul_q28_sat(i32 x, u32 gain) {
    const i32 r = mul31(x, static_cast<i32>(gain));
    return r > (0x7fffffff >> 3) ? 0x7fffffff : r < -(0x40000000 >> 2) ? static_cast<i32>(0x80000000u) : r * 8;
}

inline i32 sat_add(i32 a, i32 b) { return saturate_add(a, b); }
// a + (b - a) * frac, frac in Q31 [0, 1): in halves, so no difference overflows.
inline i32 lerp(i32 a, i32 b, i32 frac) {
    const i32 d = (b >> 1) - (a >> 1);
    return sat_add(a, mul31(d, frac) * 2);
}

// Drive: gain 1 + 5 drive/100 (up to 6x), a tanh-shaped
// curve t(v) = v (27 + v^2) / (27 + 9 v^2) that reaches +-1 with zero slope at
// |v| = 3 and holds there, then makeup 1 - 0.3 drive/100 (-3.1 dB at 100): loud
// and dirty at 100, never wrapping, never silent. 32-bit only (studio_chain.py
// drive_word is the same arithmetic):
//   v  = floor(x G / 2^31), G = 2^28 + 13421772 d   (v in Q28)
//   v2 = floor(v v / 2^31)                           (v^2 in Q25)
//   n  = floor(v (27 2^25 + v2) / 2^31)              (Q22)
//   dn = 27 2^22 + 9 (v2 >> 3)                       (Q22)
//   t  = |n| / dn to 20 bits by long division, << 11, signed (Q31)
//   out = floor(t M / 2^31), M = 2^31 - 1 - 6442450 d
const u32 kDriveGainStep = 13421772u, kDriveMakeupStep = 6442450u, kDriveBits = 20u;
// The division by a reciprocal, not 20 steps of long division (where the live chain would spend
// most of its worst blocks): 1/dn over dn's whole range [27 2^22, 108 2^22) as
// floor(2^57 / dn) at 2^20 steps, read with linear interpolation, one Newton step, then the 20-bit
// quotient made exact from its remainder r 2^20 - q dn (which fits 32 bits signed): the same
// quotient as the long division, bit for bit (tests/test_sample_studio.py BlockForm).
const u32 kDriveRecip[326] = {
    1272582902u, 1260907830u, 1249445031u, 1238188770u, 1227133513u, 1216273924u,
    1205604855u, 1195121334u, 1184818564u, 1174691910u, 1164736893u, 1154949188u,
    1145324612u, 1135859119u, 1126548798u, 1117389865u, 1108378657u, 1099511627u,
    1090785345u, 1082196484u, 1073741824u, 1065418243u, 1057222719u, 1049152316u,
    1041204192u, 1033375590u, 1025663831u, 1018066322u, 1010580540u, 1003204039u,
    995934445u, 988769449u, 981706810u, 974744350u, 967879954u, 961111562u,
    954437176u, 947854851u, 941362695u, 934958867u, 928641577u, 922409083u,
    916259689u, 910191744u, 904203641u, 898293813u, 892460736u, 886702925u,
    881018932u, 875407346u, 869866794u, 864395933u, 858993459u, 853658096u,
    848388601u, 843183763u, 838042399u, 832963354u, 827945502u, 822987745u,
    818089008u, 813248245u, 808464432u, 803736570u, 799063682u, 794444817u,
    789879042u, 785365448u, 780903144u, 776491262u, 772128952u, 767815382u,
    763549741u, 759331234u, 755159085u, 751032532u, 746950834u, 742913262u,
    738919104u, 734967665u, 731058263u, 727190230u, 723362913u, 719575672u,
    715827882u, 712118929u, 708448213u, 704815146u, 701219150u, 697659662u,
    694136128u, 690648007u, 687194767u, 683775887u, 680390858u, 677039179u,
    673720360u, 670433919u, 667179385u, 663956296u, 660764199u, 657602648u,
    654471207u, 651369447u, 648296950u, 645253302u, 642238100u, 639250946u,
    636291451u, 633359232u, 630453915u, 627575130u, 624722515u, 621895717u,
    619094385u, 616318177u, 613566756u, 610839793u, 608136962u, 605457944u,
    602802427u, 600170102u, 597560667u, 594973824u, 592409282u, 589866753u,
    587345955u, 584846610u, 582368446u, 579911196u, 577474594u, 575058382u,
    572662306u, 570286113u, 567929559u, 565592401u, 563274399u, 560975320u,
    558694932u, 556433010u, 554189328u, 551963668u, 549755813u, 547565551u,
    545392672u, 543236970u, 541098242u, 538976288u, 536870912u, 534781920u,
    532709121u, 530652330u, 528611359u, 526586028u, 524576158u, 522581572u,
    520602096u, 518637560u, 516687795u, 514752634u, 512831915u, 510925477u,
    509033161u, 507154809u, 505290270u, 503439390u, 501602019u, 499778012u,
    497967222u, 496169507u, 494384724u, 492612736u, 490853405u, 489106595u,
    487372175u, 485650012u, 483939977u, 482241942u, 480555781u, 478881370u,
    477218588u, 475567313u, 473927425u, 472298809u, 470681347u, 469074926u,
    467479433u, 465894757u, 464320788u, 462757419u, 461204541u, 459662051u,
    458129844u, 456607818u, 455095872u, 453593905u, 452101820u, 450619519u,
    449146906u, 447683887u, 446230368u, 444786257u, 443351462u, 441925895u,
    440509466u, 439102087u, 437703673u, 436314138u, 434933397u, 433561367u,
    432197966u, 430843114u, 429496729u, 428158733u, 426829048u, 425507595u,
    424194300u, 422889087u, 421591881u, 420302610u, 419021199u, 417747578u,
    416481677u, 415223424u, 413972751u, 412729590u, 411493872u, 410265532u,
    409044504u, 407830722u, 406624122u, 405424641u, 404232216u, 403046784u,
    401868285u, 400696657u, 399531841u, 398373778u, 397222408u, 396077675u,
    394939521u, 393807889u, 392682724u, 391563970u, 390451572u, 389345477u,
    388245631u, 387151981u, 386064476u, 384983062u, 383907691u, 382838310u,
    381774870u, 380717322u, 379665617u, 378619706u, 377579542u, 376545078u,
    375516266u, 374493061u, 373475417u, 372463288u, 371456631u, 370455400u,
    369459552u, 368469044u, 367483832u, 366503875u, 365529131u, 364559558u,
    363595115u, 362635761u, 361681456u, 360732161u, 359787836u, 358848442u,
    357913941u, 356984294u, 356059464u, 355139414u, 354224106u, 353313505u,
    352407573u, 351506274u, 350609575u, 349717438u, 348829831u, 347946717u,
    347068064u, 346193837u, 345324003u, 344458530u, 343597383u, 342740532u,
    341887943u, 341039586u, 340195429u, 339355440u, 338519589u, 337687846u,
    336860180u, 336036561u, 335216959u, 334401346u, 333589692u, 332781969u,
    331978148u, 331178201u, 330382099u, 329589816u, 328801324u, 328016595u,
    327235603u, 326458321u, 325684723u, 324914783u, 324148475u, 323385772u,
    322626651u, 321871085u, 321119050u, 320370520u, 319625473u, 318883882u,
    318145725u, 317410977u};
const u32 kDriveRecipBase = 27u << 22;
inline i32 drive_word(i32 x, u32 drive) {
    const i32 g = static_cast<i32>(kUnity + kDriveGainStep * drive);
    const i32 v = mul31(x, g);
    const i32 three = static_cast<i32>(3u << 28);
    i32 t;
    if (v >= three) {
        t = 0x7fffffff;
    } else if (v <= -three) {
        t = -0x7fffffff;
    } else {
        const i32 v2 = mul31(v, v);
        const i32 n = mul31(v, static_cast<i32>(27u << 25) + v2);
        const u32 dn = (27u << 22) + 9u * static_cast<u32>(v2 >> 3);
        u32 r = static_cast<u32>(n < 0 ? -n : n);
        if (r >= dn) r = dn - 1;   // rounding just under |v| = 3: the curve's top, not past it
        const u32 at = dn - kDriveRecipBase, index = at >> 20;
        const i32 high = static_cast<i32>(kDriveRecip[index]), low = static_cast<i32>(kDriveRecip[index + 1]);
        i32 recip = high - mul31(high - low, static_cast<i32>((at & 0xfffffu) << 11));   // about 2^57 / dn
        const i32 p = mul31(static_cast<i32>(dn << 2), recip);                           // about 2^28
        recip = mul31(recip, static_cast<i32>((1u << 29) - static_cast<u32>(p))) << 3;  // Newton: x (2 - d x)
        u32 q = static_cast<u32>(mul31(static_cast<i32>(r << 2), recip)) >> 8;          // r 2^20 / dn, within one
        i32 rem = static_cast<i32>((r << 20) - q * dn);                                    // modulo 2^32: exact here
        if (rem < 0) { --q; rem += static_cast<i32>(dn); } else if (rem >= static_cast<i32>(dn)) { ++q; rem -= static_cast<i32>(dn); }
        if (rem < 0) --q; else if (rem >= static_cast<i32>(dn)) ++q;
        const i32 m = static_cast<i32>(q << (31u - kDriveBits));
        t = n < 0 ? -m : m;
    }
    return mul31(t, static_cast<i32>(0x7fffffffu - kDriveMakeupStep * drive));
}

struct Settings {
    // the trim page (source and writer)
    u32 begin, end, reverse;
    u32 gain;                 // Q4.28 on the 16-bit samples
    u32 dc;
    // the time page
    u32 stretch;              // percent, 25..400; 100 none
    i32 pitch;                // semitones, -24..24
    u32 beat;                 // 0 tape (speed and pitch together), 1 beat (pitch kept)
    u32 grain_ms;             // beat's grain, 20..120
    // the process page
    u32 rate_index, bits_index, drive;
    i32 sweep;                // -48..-1 low-pass at cutoff -sweep, 0 off, 1..48 high-pass
    u32 comp;                 // 0..100
};
inline bool time_active(const Settings& s) { return s.stretch != 100 || s.pitch != 0; }
inline bool color_active(const Settings& s) {
    return s.rate_index || s.bits_index || s.drive || s.sweep || s.comp;
}

// Beat's grain: the page's grain, grown with the compression the
// grains must make (stretch x pitch ratio below 1: input per output above 1), so a x0.25
// edit plays longer, continuous pieces instead of 20 ms slices, up to kMaxGrainMs.
const u32 kMaxGrainMs = 160;
inline u32 grain_ms_for(const Settings& s) {
    const u32 page = s.grain_ms < 20 ? 20 : s.grain_ms > 120 ? 120 : s.grain_ms;
    const u32 ratio = kSemitoneQ16[(s.pitch < -24 ? -24 : s.pitch > 24 ? 24 : s.pitch) + 24];
    const u32 percent = (s.stretch * ratio) >> 16;   // the grains' own stretch, percent
    if (percent >= 100) return page;
    const u32 grown = percent ? page * 100u / percent : kMaxGrainMs;
    return grown > kMaxGrainMs ? (page > kMaxGrainMs ? page : kMaxGrainMs) : grown;
}

struct Pipeline {
    // ---- source: the frames between the markers, reversed if asked, with gain
    const i16* data;
    u32 channels, rate;
    Settings s;
    u32 length;               // source frames, end - begin
    i32 at(u32 i, u32 c) const {
        const u32 f = s.reverse ? s.end - 1 - i : s.begin + i;
        const i32 x = data[f * channels + c];
        return s.gain == kUnity ? x * 65536 : gain_word(x, s.gain);
    }
    // ---- beat: WSOLA over the source, pitch kept
    u32 grain, hop;           // frames: grain, and hop = grain / 2
    u32 step_q31;             // floor(2^31 / hop): the crossfade's slope
    u32 next_int, next_frac;  // the next analysis position, Q16 fraction
    u32 advance_int, advance_frac;  // input per hop, as Q16 parts
    u32 previous;             // start of the grain being faded out
    u32 current;              // start of the grain being faded in
    u32 index;                // frame within the hop
    bool beat_on, first, beat_done;
    // the search's probes, made once a hop: hop <= 3840 (160 ms at 48 kHz) gives 240 terms,
    // span <= 256 gives 65 candidates, so 65 + 2 * 239 = 543 grid slots
    static constexpr u32 kProbeTerms = 240, kProbeSlots = 544;
    i32 natural_probes[kProbeTerms];
    i32 grid_probes[kProbeSlots];
    // ---- the resampler: tape speed, or beat pitch
    u32 speed_q16;            // input frames per output frame, Q16; 65536 none
    u32 pos_int, pos_frac;
    i32 window[2][2];         // upstream frames pos_int and pos_int + 1
    u32 loaded;               // upstream frames pulled so far
    bool upstream_done;
    // ---- the source read in order (when no beat stage)
    u32 read;
    // ---- color
    u32 rate_acc;
    bool held_valid;
    i32 held[2];
    i32 low[2], band[2];
    u32 bits_env;             // the peak the bit depth's step follows
    u32 comp_env, comp_gain, comp_count;   // comp_gain in Q20
    // ---- DC
    DcBlock block;

    void start(const i16* pcm, u32 chans, u32 sample_rate, const Settings& settings) {
        data = pcm;
        channels = chans;
        rate = sample_rate;
        s = settings;
        length = s.end > s.begin ? s.end - s.begin : 0;
        read = 0;
        // beat
        u32 ratio = kSemitoneQ16[(s.pitch < -24 ? -24 : s.pitch > 24 ? 24 : s.pitch) + 24];
        beat_on = s.beat && time_active(s);
        if (beat_on) {
            grain = rate * grain_ms_for(s) / 1000;
            grain &= ~1u;
            if (grain < 64) grain = 64;
            hop = grain / 2;
            step_q31 = 0x7fffffffu / hop;   // hop * step stays under 2^31
            // input per output = 1 / (stretch * ratio): Q16 = (100 << 24) / (stretch * ratio >> 8)
            const u32 denom = (s.stretch * ratio) >> 8;
            const u32 per_out_q16 = denom ? (6553600u << 8) / denom : 65536u;
            // hop * per_out_q16 in two parts: hop <= 3840 (160 ms at 48 kHz) and per_out_q16 up to 2^20
            // would pass 2^32 whole
            const u32 low = hop * (per_out_q16 & 0xffffu);
            advance_int = hop * (per_out_q16 >> 16) + (low >> 16);
            advance_frac = low & 0xffffu;
            next_int = next_frac = 0;
            previous = current = 0;
            index = 0;
            first = true;
            beat_done = false;
            speed_q16 = ratio;   // then resample by the pitch ratio
        } else {
            // tape: speed and pitch together, from the stretch alone (the page ties pitch to it)
            speed_q16 = s.stretch == 100 ? 65536u : 6553600u / s.stretch;
        }
        pos_int = pos_frac = 0;
        loaded = 0;
        upstream_done = false;
        // color
        rate_acc = 0;
        held_valid = false;
        held[0] = held[1] = 0;
        low[0] = low[1] = band[0] = band[1] = 0;
        bits_env = 0;
        comp_env = 0;
        comp_gain = 1u << 20;
        comp_count = 0;
        if (s.dc) block.start(rate);
    }

    // The match signal: both channels of a stereo source (the left alone
    // leaves the right channel's splices unaligned), 12 bits, so a hop's sum stays in 32 bits.
    i32 probe(u32 i) const {
        return channels > 1 ? (at(i, 0) >> 21) + (at(i, 1) >> 21) : at(i, 0) >> 20;
    }
    // The beat stage: one frame, crossfading grains at each hop.
    bool beat_frame(i32* out) {
        if (beat_done) return false;
        if (index == 0) {
            if (first) {
                current = 0;
            } else {
                // the next analysis position, then the best match near it
                next_int += advance_int + ((next_frac + advance_frac) >> 16);
                next_frac = (next_frac + advance_frac) & 0xffffu;
                previous = current;
                const u32 natural = previous + hop;
                const u32 nominal = next_int;
                if (nominal + hop > length || natural + hop > length) {
                    beat_done = true;
                    return false;
                }
                const u32 span = hop / 2 < 256 ? hop / 2 : 256;
                u32 best = nominal;
                i32 best_score = -0x7fffffff;
                // Every probe a candidate reads lies on one 8-frame grid from nominal - span,
                // so each grid point's probe is made once a hop, and the natural continuation's once, instead of
                // both for every candidate (~7,000 probes a hop at 40 ms to ~230). The same products are summed
                // in the same order: the choice is bit for bit the one a per-candidate search makes.
                const u32 terms = (hop + 15) / 16;              // j = 0, 16, ... < hop
                const u32 candidates = 2 * span / 8 + 1;        // d = -span, -span + 8, ... <= span
                const u32 slots = candidates + 2 * (terms - 1); // candidate k's term m reads slot k + 2m
                if (terms <= kProbeTerms && slots <= kProbeSlots) {
                    const i32 base = static_cast<i32>(nominal) - static_cast<i32>(span);
                    for (u32 m = 0; m < terms; ++m) natural_probes[m] = probe(natural + 16 * m);
                    for (u32 t = 0; t < slots; ++t) {
                        const i32 at_t = base + static_cast<i32>(8 * t);
                        grid_probes[t] = at_t >= 0 && static_cast<u32>(at_t) < length ? probe(static_cast<u32>(at_t)) : 0;
                    }
                    for (u32 k = 0; k < candidates; ++k) {
                        const i32 c = base + static_cast<i32>(8 * k);
                        if (c < 0 || static_cast<u32>(c) + hop > length) continue;
                        i32 score = 0;
                        for (u32 m = 0; m < terms; ++m) score += grid_probes[k + 2 * m] * natural_probes[m];
                        if (score > best_score) {
                            best_score = score;
                            best = static_cast<u32>(c);
                        }
                    }
                } else {
                    for (i32 d = -static_cast<i32>(span); d <= static_cast<i32>(span); d += 8) {
                        const i32 c = static_cast<i32>(nominal) + d;
                        if (c < 0 || static_cast<u32>(c) + hop > length) continue;
                        i32 score = 0;
                        for (u32 j = 0; j < hop; j += 16)
                            score += probe(static_cast<u32>(c) + j) * probe(natural + j);
                        if (score > best_score) {
                            best_score = score;
                            best = static_cast<u32>(c);
                        }
                    }
                }
                current = best;
            }
        }
        for (u32 c = 0; c < channels; ++c) {
            if (first) {
                out[c] = at(index, c);
            } else {
                const i32 fade_in = static_cast<i32>(index * step_q31);
                const i32 fade_out = static_cast<i32>((hop - index) * step_q31);
                out[c] = sat_add(mul31(at(previous + hop + index, c), fade_out), mul31(at(current + index, c), fade_in));
            }
        }
        if (++index == hop) {
            index = 0;
            first = false;
        }
        return true;
    }
    bool upstream(i32* out) {
        if (beat_on) return beat_frame(out);
        if (read >= length) return false;
        for (u32 c = 0; c < channels; ++c) out[c] = at(read, c);
        ++read;
        return true;
    }
    // The resampler: linear between upstream frames.
    bool timed(i32* out) {
        if (speed_q16 == 65536u) return upstream(out);
        while (loaded < pos_int + 2) {
            i32 f[2] = {0, 0};
            if (upstream_done || !upstream(f)) {
                upstream_done = true;
                break;
            }
            const u32 slot = loaded & 1u;
            for (u32 c = 0; c < channels; ++c) window[slot][c] = f[c];
            ++loaded;
        }
        if (pos_int + 2 > loaded) return false;
        const u32 a = pos_int & 1u, b = (pos_int + 1) & 1u;
        for (u32 c = 0; c < channels; ++c)
            out[c] = lerp(window[a][c], window[b][c], static_cast<i32>(pos_frac << 15));
        pos_frac += speed_q16 & 0xffffu;
        pos_int += (speed_q16 >> 16) + (pos_frac >> 16);
        pos_frac &= 0xffffu;
        return true;
    }
    // Color: drive, bits, rate, the filter sweep, comp.
    void color(i32* w) {
        const u32 ch = channels < 2 ? 1u : 2u;
        if (s.drive)
            for (u32 c = 0; c < ch; ++c) w[c] = drive_word(w[c], s.drive);
        if (s.bits_index) {
            u32 m = 0;
            for (u32 c = 0; c < ch; ++c) {
                const u32 a = magnitude(w[c]);
                if (a > m) m = a;
            }
            if (m > bits_env) bits_env = m;
            else bits_env -= bits_env >> kBitsRelease;
            const u32 top = top_bit(bits_env | kBitsFloor);   // the envelope's top bit
            const u32 b = kBits[s.bits_index];
            if (top + 1u > b) {
                const u32 shift = top + 1u - b;
                const i32 half = static_cast<i32>(1u << (shift - 1u));
                const u32 mask = ~((1u << shift) - 1u);
                for (u32 c = 0; c < ch; ++c) w[c] = static_cast<i32>(static_cast<u32>(sat_add(w[c], half)) & mask);
            }
        }
        if (s.rate_index && kRateHz[s.rate_index] < rate) {
            // sample and hold: a new sample each time the accumulator passes the rate
            rate_acc += kRateHz[s.rate_index];
            if (!held_valid || rate_acc >= rate) {
                if (held_valid) rate_acc -= rate;
                held[0] = w[0];
                held[1] = w[1];
                held_valid = true;
            }
            w[0] = held[0];
            w[1] = held[1];
        }
        if (s.sweep) {
            const u32 index = static_cast<u32>(s.sweep < 0 ? -s.sweep : s.sweep);
            const i32 f = static_cast<i32>(kCutoffQ31[(index > kCutoffCount ? kCutoffCount : index) - 1]);
            for (u32 c = 0; c < ch; ++c) {
                const i32 x = w[c] >> 2;
                i32 high = 0;
                for (u32 pass = 0; pass < 2; ++pass) {
                    low[c] = sweep_clamp(sat_add(low[c], mul31(f, band[c])));
                    const i32 damp = mul31(kSweepHalf, band[c]);
                    high = sweep_clamp(sat_add(sat_add(x, -low[c]), -sat_add(damp, damp)));
                    band[c] = sweep_clamp(sat_add(band[c], mul31(f, high)));
                }
                const i32 y = s.sweep < 0 ? low[c] : high;
                w[c] = y >= (1 << 29) ? 0x7fffffff : y < -(1 << 29) ? static_cast<i32>(0x80000000u) : y * 4;
            }
        }
        if (s.comp) {
            const u32 a = s.comp > 100 ? 100u : s.comp;
            u32 m = 0;
            for (u32 c = 0; c < ch; ++c) {
                const u32 v = magnitude(w[c]);
                if (v > m) m = v;
            }
            if (m > comp_env) comp_env += (m - comp_env) >> kCompAttack;
            else comp_env -= comp_env >> kCompRelease;
            if (comp_count++ % kCompEvery == 0) {
                const u32 t = kCompThreshold[a], env = comp_env;
                if (env <= t) {
                    comp_gain = 1u << 20;
                } else {
                    const u32 over = env - t, inv = kCompInverse[a];
                    const u32 num = t + (over >> 16) * inv + (((over & 0xffffu) * inv) >> 16);
                    comp_gain = num < env ? frac20(num, env) : 1u << 20;
                }
            }
            for (u32 c = 0; c < ch; ++c) {
                i32 x = w[c];
                if (comp_gain < (1u << 20)) x = mul31(x, static_cast<i32>(comp_gain << 11));
                w[c] = mul_q28_sat(x, kCompMakeup[a]);
            }
        }
        if (ch < 2) w[1] = w[0];
    }
    // The colour stage over a block, each step across the block in turn: per frame
    // the same arithmetic in the same order as color() (the steps that couple the channels take both
    // per frame), so the same words, at a fraction of the cost per frame in the audio block.
    void color_block(i32* l, i32* r, u32 n) {
        const u32 ch = channels < 2 ? 1u : 2u;
        i32* const w[2] = {l, r};
        if (s.drive) {
            const u32 d = s.drive;
            for (u32 c = 0; c < ch; ++c)
                for (u32 k = 0; k < n; ++k) w[c][k] = drive_word(w[c][k], d);
        }
        if (s.bits_index) {
            const u32 b = kBits[s.bits_index];
            for (u32 k = 0; k < n; ++k) {
                u32 m = magnitude(l[k]);
                if (ch > 1) {
                    const u32 a = magnitude(r[k]);
                    if (a > m) m = a;
                }
                if (m > bits_env) bits_env = m;
                else bits_env -= bits_env >> kBitsRelease;
                const u32 top = top_bit(bits_env | kBitsFloor);
                if (top + 1u > b) {
                    const u32 shift = top + 1u - b;
                    const i32 half = static_cast<i32>(1u << (shift - 1u));
                    const u32 mask = ~((1u << shift) - 1u);
                    l[k] = static_cast<i32>(static_cast<u32>(sat_add(l[k], half)) & mask);
                    if (ch > 1) r[k] = static_cast<i32>(static_cast<u32>(sat_add(r[k], half)) & mask);
                }
            }
        }
        if (s.rate_index && kRateHz[s.rate_index] < rate) {
            const u32 hz = kRateHz[s.rate_index];
            for (u32 k = 0; k < n; ++k) {
                rate_acc += hz;
                if (!held_valid || rate_acc >= rate) {
                    if (held_valid) rate_acc -= rate;
                    held[0] = l[k];
                    held[1] = r[k];
                    held_valid = true;
                }
                l[k] = held[0];
                r[k] = held[1];
            }
        }
        if (s.sweep) {
            const u32 index = static_cast<u32>(s.sweep < 0 ? -s.sweep : s.sweep);
            const i32 f = static_cast<i32>(kCutoffQ31[(index > kCutoffCount ? kCutoffCount : index) - 1]);
            const bool low_pass = s.sweep < 0;
            for (u32 c = 0; c < ch; ++c) {
                i32 lo = low[c], bd = band[c];
                i32* const x = w[c];
                for (u32 k = 0; k < n; ++k) {
                    const i32 in = x[k] >> 2;
                    i32 high = 0;
                    for (u32 pass = 0; pass < 2; ++pass) {
                        lo = sweep_clamp(sat_add(lo, mul31(f, bd)));
                        const i32 damp = mul31(kSweepHalf, bd);
                        high = sweep_clamp(sat_add(sat_add(in, -lo), -sat_add(damp, damp)));
                        bd = sweep_clamp(sat_add(bd, mul31(f, high)));
                    }
                    const i32 y = low_pass ? lo : high;
                    x[k] = y >= (1 << 29) ? 0x7fffffff : y < -(1 << 29) ? static_cast<i32>(0x80000000u) : y * 4;
                }
                low[c] = lo;
                band[c] = bd;
            }
        }
        if (s.comp) {
            const u32 a = s.comp > 100 ? 100u : s.comp;
            const u32 t = kCompThreshold[a], inv = kCompInverse[a], makeup = kCompMakeup[a];
            for (u32 k = 0; k < n; ++k) {
                u32 m = magnitude(l[k]);
                if (ch > 1) {
                    const u32 v = magnitude(r[k]);
                    if (v > m) m = v;
                }
                if (m > comp_env) comp_env += (m - comp_env) >> kCompAttack;
                else comp_env -= comp_env >> kCompRelease;
                if (comp_count++ % kCompEvery == 0) {
                    const u32 env = comp_env;
                    if (env <= t) {
                        comp_gain = 1u << 20;
                    } else {
                        const u32 over = env - t;
                        const u32 num = t + (over >> 16) * inv + (((over & 0xffffu) * inv) >> 16);
                        comp_gain = num < env ? frac20(num, env) : 1u << 20;
                    }
                }
                for (u32 c = 0; c < ch; ++c) {
                    i32 x = w[c][k];
                    if (comp_gain < (1u << 20)) x = mul31(x, static_cast<i32>(comp_gain << 11));
                    w[c][k] = mul_q28_sat(x, makeup);
                }
            }
        }
        if (ch < 2)
            for (u32 k = 0; k < n; ++k) r[k] = l[k];
    }
    // Up to n output frames into l and r, the block form of next(): the same
    // words, frame for frame. Returns how many it made (fewer at the end).
    u32 fill(i32* l, i32* r, u32 n) {
        u32 k = 0;
        for (; k < n; ++k) {
            i32 w[2] = {0, 0};
            if (!timed(w)) break;
            l[k] = w[0];
            r[k] = channels < 2 ? w[0] : w[1];
        }
        if (color_active(s)) color_block(l, r, k);
        if (s.dc)
            for (u32 c = 0; c < channels && c < 2; ++c) {
                i32* const x = c ? r : l;
                for (u32 j = 0; j < k; ++j) x[j] = block.process(c, x[j]);
            }
        return k;
    }
    // One output frame before the fades and channels, or false at the end.
    bool next(i32* out) {
        out[0] = out[1] = 0;
        if (!timed(out)) return false;
        if (channels < 2) out[1] = out[0];
        if (color_active(s)) color(out);
        if (s.dc)
            for (u32 c = 0; c < channels; ++c) out[c] = block.process(c, out[c]);
        return true;
    }
};
}  // namespace
}}
