// BCPack decoder for Xbox Series DirectStorage textures.
// Adapted from bcpack-rs by Ahmet Dedeler (https://github.com/Ahmet-Dedeler, https://ahmetdedeler.com).
// Source: https://github.com/Ahmet-Dedeler/bcpack-rs
// SPDX-License-Identifier: MIT OR Apache-2.0
// Incorporates rANS interval models derived from Microsoft silicon-rans (MIT License).
#pragma once

#include <cstdint>
#include <cstring>
#include <vector>
#include <utility>
#include <algorithm>
#include <intrin.h>

#include "bcpack_tables.h"

namespace x2w_bcpack {

inline void model_forward(uint8_t eab, uint8_t q, uint16_t sym, uint32_t& f, uint32_t& cf) {
    uint32_t s = sym;
    unsigned long seg = 0;
    if (s != 0) {
        _BitScanReverse(&seg, s);
        seg += 1;
    }
    uint32_t st = (seg == 0) ? 0 : (1u << (seg - 1));
    uint32_t fa = FREQ_TABLE[eab][q][seg];
    uint32_t cfa = (BASE_TABLE[eab][q][seg] + fa * (s - st)) & 0xFFFF;
    if (cfa == 0) {
        f = (fa - 1) & 0xFFFF;
        cf = 1;
    } else {
        f = fa;
        cf = cfa;
    }
}

inline uint32_t model_advance(uint8_t eab, uint8_t q, uint32_t x, uint16_t sym) {
    uint32_t f, cf;
    model_forward(eab, q, sym, f, cf);
    return f * (x >> 16) + (x & 0xFFFF) - cf;
}

inline uint16_t model_inverse_fast(uint8_t eab, uint8_t q, uint16_t cf) {
    uint32_t c = cf;
    uint32_t seg = 0;
    uint32_t nseg = eab + 2;
    for (uint32_t k = 1; k < nseg; ++k) {
        if (c >= BASE_TABLE[eab][q][k]) {
            seg = k;
        } else {
            break;
        }
    }
    uint32_t base_x = (seg == 0) ? 0 : (1u << (seg - 1));
    uint32_t dist = (c - BASE_TABLE[eab][q][seg] + RN_TABLE[eab][q][seg]) & 0xFFFF;
    uint32_t mul = dist * RF_TABLE[eab][q][seg];
    uint32_t steps = ((mul >> 15) >> RS_TABLE[eab][q][seg]) & 0x1FF;
    return static_cast<uint16_t>((base_x + steps) & 0x1FF);
}

inline int32_t unzigzag(uint16_t v) {
    int32_t iv = static_cast<int32_t>(v);
    return (iv & 1) ? -((iv + 1) >> 1) : (iv >> 1);
}

inline uint8_t residual(uint16_t sym, uint32_t prec) {
    int32_t unz = unzigzag(sym);
    return static_cast<uint8_t>(unz & ((int32_t{1} << prec) - 1));
}

inline uint8_t interleave(uint16_t even, uint16_t odd) {
    uint8_t out = 0;
    for (int k = 0; k < 4; ++k) {
        out |= static_cast<uint8_t>(((even >> k) & 1) << (2 * k));
        out |= static_cast<uint8_t>(((odd >> k) & 1) << (2 * k + 1));
    }
    return out;
}

inline void deinterleave(uint8_t b, uint16_t& even, uint16_t& odd) {
    even = 0;
    odd = 0;
    for (int k = 0; k < 4; ++k) {
        even |= static_cast<uint16_t>(((b >> (2 * k)) & 1) << k);
        odd |= static_cast<uint16_t>(((b >> (2 * k + 1)) & 1) << k);
    }
}

inline void index_pair(uint16_t sym, size_t row, uint16_t& left, uint16_t& right) {
    uint16_t even, odd;
    deinterleave(static_cast<uint8_t>(sym & 0xFF), even, odd);
    if (row < 3) {
        left = odd;
        right = even;
    } else {
        left = even;
        right = odd;
    }
}

enum LaneState : uint8_t {
    LANE_IDLE = 0,
    LANE_HDR0 = 1,
    LANE_HDR1 = 2,
    LANE_HDR2 = 3,
    LANE_PROC = 4,
    LANE_DRAIN = 5,
};

struct Lane {
    uint8_t ab = 0;
    uint32_t target = 0;
    LaneState state = LANE_IDLE;
    uint8_t ib = 0;
    bool ib_full = false;
    uint16_t sym = 0;
    bool sym_full = false;
    uint32_t in_rem = 0;
    uint32_t out_rem = 0;
    uint32_t p0 = 0;
    uint32_t p1 = 0;
    bool hdr3 = false;
    bool flush = false;
    uint8_t virtual_zeros = 0;
    uint8_t q = 0;
    uint8_t eab = 0;
    bool done = false;
    bool go = true;
    bool stall = false;
    uint32_t nsyms = 0;
    uint32_t nbytes = 0;
    uint32_t nfrags = 0;
    uint32_t final_state = 0;
};

inline bool phase1(Lane& ln) {
    switch (ln.state) {
    case LANE_IDLE:
        if (ln.go) {
            ln.go = false;
            ln.state = LANE_HDR0;
            ln.out_rem = ln.target;
            ln.p0 = 0;
            ln.p1 = 0;
        }
        break;
    case LANE_HDR0:
        if (ln.ib_full) {
            uint8_t b = ln.ib;
            ln.ib_full = false;
            ln.in_rem = 0;
            ln.state = LANE_HDR1;
            ln.virtual_zeros = 0;
            ln.q = b & 15;
            ln.hdr3 = (b & 0x20) != 0;
            ln.flush = (b & 0x10) != 0;
            int32_t e = (static_cast<int32_t>(ln.ab) - static_cast<int32_t>(b >> 6)) & 15;
            ln.eab = (e < 10) ? static_cast<uint8_t>(e) : 9;
            ln.nfrags++;
            if (ln.eab > 8) return false;
        }
        break;
    case LANE_HDR1:
        if (ln.ib_full) {
            ln.ib_full = false;
            if (ln.hdr3) {
                ln.in_rem = ln.ib;
                ln.state = LANE_HDR2;
            } else {
                ln.in_rem = static_cast<uint32_t>(ln.ib) + 1;
                ln.state = LANE_PROC;
            }
        }
        break;
    case LANE_HDR2:
        if (ln.ib_full) {
            ln.ib_full = false;
            ln.in_rem = ((static_cast<uint32_t>(ln.ib) << 8) | (ln.in_rem & 0xFF)) + 257;
            ln.state = LANE_PROC;
        }
        break;
    default: {
        bool is_proc = (ln.state == LANE_PROC);
        bool will = is_proc && (ln.p0 < 0x1000000) && (ln.in_rem > 0) && ln.ib_full;
        uint32_t x = will ? (((ln.p0 & 0xFFFFFF) << 8) | ln.ib) : ln.p0;
        uint32_t remaining = ln.in_rem - (will ? 1 : 0);
        if (ln.flush && remaining == 0) {
            while (x > 0 && x < 0x1000000 && ln.virtual_zeros < 2) {
                x <<= 8;
                ln.virtual_zeros++;
            }
        }
        bool nsf = (x > 0x1000000) && (ln.out_rem > 0);
        if (nsf) {
            ln.out_rem--;
            ln.sym = model_inverse_fast(ln.eab, ln.q, static_cast<uint16_t>(x & 0xFFFF));
        }
        uint32_t new_out = ln.out_rem;
        ln.p1 = x;
        ln.sym_full = nsf;
        if (is_proc) {
            if (will) {
                ln.in_rem--;
                ln.ib_full = false;
            }
            if (ln.in_rem == 0) {
                if (new_out > 0) {
                    if (x == 0x1000000) {
                        ln.state = LANE_HDR0;
                        ln.virtual_zeros = 0;
                        if (ln.flush) ln.p1 = 0;
                    } else {
                        ln.state = LANE_DRAIN;
                    }
                } else {
                    ln.done = true;
                    ln.state = LANE_IDLE;
                    ln.final_state = nsf ? model_advance(ln.eab, ln.q, x, ln.sym) : x;
                }
            }
        } else if (x <= 0x1000000) {
            if (new_out > 0) {
                // A flushed fragment must return to the rANS home state (0x1000000).
                // Merely being <= L could silently accept a damaged fragment.
                if (ln.flush && x != 0x1000000) return false;
                ln.state = LANE_HDR0;
                ln.virtual_zeros = 0;
                if (ln.flush) {
                    ln.p0 = 0;
                    ln.p1 = 0;
                }
            } else {
                ln.state = LANE_IDLE;
                ln.done = true;
                ln.final_state = nsf ? model_advance(ln.eab, ln.q, x, ln.sym) : x;
            }
        }
        break;
    }
    }
    return true;
}

inline bool decode_lanes(const uint8_t* input, size_t input_size,
                         const uint8_t* alphabet_bits, size_t num_lanes,
                         size_t target_symbols_per_lane,
                         std::vector<uint16_t>& symbols) {
    if (num_lanes == 0 || num_lanes > 16 || target_symbols_per_lane == 0) return false;
    for (size_t i = 0; i < num_lanes; ++i) {
        if (alphabet_bits[i] > 8) return false;
    }
    symbols.assign(num_lanes * target_symbols_per_lane, 0);

    std::vector<Lane> lanes(num_lanes);
    for (size_t i = 0; i < num_lanes; ++i) {
        lanes[i].ab = alphabet_bits[i];
        lanes[i].target = static_cast<uint32_t>(target_symbols_per_lane);
    }

    uint32_t slots = 0;
    size_t pos = 0;
    uint32_t exhausted_mask = 0;
    uint64_t cycle = 0;
    uint64_t max_cycles = 64ull * (num_lanes * target_symbols_per_lane + input_size) + 1000;
    uint64_t total_syms = 0;
    size_t last_pos = SIZE_MAX;
    uint64_t last_total_syms = UINT64_MAX;
    uint32_t last_state_sum = UINT32_MAX;
    uint64_t last_change = 0;

    while (cycle < max_cycles) {
        size_t served = 0;
        for (size_t i = 0; i < num_lanes; ++i) {
            Lane& ln = lanes[i];
            uint32_t bit = 1u << i;
            bool blocked_out = ln.sym_full && ((slots & bit) != 0);
            bool need = !ln.ib_full && ((ln.state >= LANE_HDR0 && ln.state <= LANE_HDR2) ||
                                       (ln.state == LANE_PROC && ln.in_rem > 0));
            bool valid = false;
            uint8_t byte = 0;
            if (need && !blocked_out && served < 8 && (exhausted_mask & bit) == 0) {
                if (pos < input_size) {
                    valid = true;
                    byte = input[pos];
                } else {
                    exhausted_mask |= bit;
                }
            }
            ln.stall = (need && !valid) || blocked_out;
            if (ln.stall) continue;
            if (ln.state == LANE_PROC || ln.state == LANE_DRAIN) {
                ln.p0 = ln.sym_full ? model_advance(ln.eab, ln.q, ln.p1, ln.sym) : ln.p1;
            }
            if (valid) {
                ln.ib = byte;
                ln.ib_full = true;
                pos++;
                served++;
                ln.nbytes++;
            }
            if (ln.sym_full) {
                slots |= bit;
                symbols[i * target_symbols_per_lane + ln.nsyms] = ln.sym;
                ln.nsyms++;
                total_syms++;
                ln.sym_full = false;
            }
        }

        uint32_t active = 0;
        bool finished = true;
        uint32_t state_sum = 0;
        for (size_t i = 0; i < num_lanes; ++i) {
            Lane& ln = lanes[i];
            if (!ln.stall) {
                if (!phase1(ln)) return false;
            }
            uint32_t bit = 1u << i;
            if (!(ln.done && !ln.sym_full && (slots & bit) == 0)) {
                active |= bit;
            }
            finished &= (ln.done && !ln.sym_full);
            state_sum += ln.state;
        }

        if (slots != 0 && (slots & active) == active) {
            slots = 0;
        }
        cycle++;
        if (finished) {
            for (size_t i = 0; i < num_lanes; ++i) {
                uint32_t f = lanes[i].final_state;
                if (f != 0x1000000 && f != 0x10000 && f != 0x100) {
                    return false;
                }
            }
            for (size_t p = pos; p < input_size; ++p) {
                if (input[p] != 0) return false;
            }
            return true;
        }

        if (pos != last_pos || total_syms != last_total_syms || state_sum != last_state_sum) {
            last_pos = pos;
            last_total_syms = total_syms;
            last_state_sum = state_sum;
            last_change = cycle;
        } else if (cycle - last_change > 64) {
            return false;
        }
    }
    return false;
}

inline constexpr uint8_t SUBSETS[8] = { 3, 2, 3, 2, 1, 1, 1, 2 };
inline constexpr uint8_t CHANNEL_BITS[8][4] = {
    { 4, 4, 4, 0 },
    { 6, 6, 6, 0 },
    { 5, 5, 5, 0 },
    { 7, 7, 7, 0 },
    { 5, 5, 5, 6 },
    { 7, 7, 7, 8 },
    { 7, 7, 7, 7 },
    { 5, 5, 5, 5 },
};
inline constexpr uint8_t INDEX_BITS[8][2] = {
    { 3, 0 },
    { 3, 0 },
    { 2, 0 },
    { 2, 0 },
    { 3, 2 },
    { 2, 2 },
    { 4, 0 },
    { 2, 0 },
};
inline constexpr uint8_t PARTITION_BITS[8] = { 4, 6, 6, 6, 0, 0, 0, 6 };
inline constexpr uint8_t ANCHOR_2[64] = {
    15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
    15, 2, 8, 2, 2, 8, 8, 15, 2, 8, 2, 2, 8, 8, 2, 2,
    15, 15, 6, 8, 2, 8, 15, 15, 2, 8, 2, 2, 2, 15, 15, 6,
    6, 2, 6, 8, 15, 15, 2, 2, 15, 15, 15, 15, 15, 2, 2, 15
};
inline constexpr uint8_t ANCHOR_3A[64] = {
    3, 3, 15, 15, 8, 3, 15, 15, 8, 8, 6, 6, 6, 5, 3, 3,
    3, 3, 8, 15, 3, 3, 6, 10, 5, 8, 8, 6, 8, 5, 15, 15,
    8, 15, 3, 5, 6, 10, 8, 15, 15, 3, 15, 5, 15, 15, 15, 15,
    3, 15, 5, 5, 5, 8, 5, 10, 5, 10, 8, 13, 15, 12, 3, 3
};
inline constexpr uint8_t ANCHOR_3B[64] = {
    15, 8, 8, 3, 15, 15, 3, 8, 15, 15, 15, 15, 15, 15, 15, 8,
    15, 8, 15, 3, 15, 8, 15, 8, 3, 15, 6, 10, 15, 15, 10, 8,
    15, 3, 15, 10, 10, 8, 9, 10, 6, 15, 8, 15, 3, 6, 6, 8,
    15, 3, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3, 15, 15, 8
};
inline constexpr int INDEX_LANES[2][4] = {
    { 11, 12, 13, 14 },
    { 6, 7, 8, 10 }
};

inline int endpoint_lane(size_t subsets, size_t row, size_t ch) {
    if (subsets == 3 && ch == 3 && (row == 0 || row == 1)) return -1;
    if (row == 0) return static_cast<int>(2 + ch);
    if (row == 1) return static_cast<int>(6 + ch);
    if (row == 2 && ch == 0) return 10;
    if (row == 2 && subsets == 3 && ch == 1) return 5;
    if (row == 2 && subsets == 3 && ch == 2) return 9;
    return -1;
}

struct BitWriter128 {
    uint64_t low = 0;
    uint64_t high = 0;
    uint32_t at = 0;

    void put(uint32_t val, uint32_t n) {
        if (n == 0) return;
        uint64_t v = (n == 64) ? val : (val & ((1ull << n) - 1));
        if (at < 64) {
            low |= (v << at);
            if (at + n > 64) {
                high |= (v >> (64 - at));
            }
        } else if (at < 128) {
            high |= (v << (at - 64));
        }
        at += n;
    }

    void write_to(uint8_t* out) const {
        std::memcpy(out, &low, 8);
        std::memcpy(out + 8, &high, 8);
    }
};

struct Bc7State {
    uint8_t subsets = 0;
    uint8_t ep[3][8] = {};
    uint8_t ix[2][16] = {};
};

inline void pack_bc7_block(const Bc7Control& c, const Bc7State& st, uint8_t* out) {
    uint32_t mode = c.mode;
    uint32_t subsets = SUBSETS[mode];
    uint8_t prec[4];
    for (int ch = 0; ch < 4; ++ch) prec[ch] = CHANNEL_BITS[mode][ch];
    if (c.rotation != 0) {
        std::swap(prec[3], prec[c.rotation - 1]);
    } else if (mode == 0 || mode == 1 || mode == 3 || mode == 6 || mode == 7) {
        prec[0] += 1;
    }
    bool has_pbits = (mode == 0 || mode == 1 || mode == 3 || mode == 6 || mode == 7);

    BitWriter128 w;
    w.put(1u << mode, mode + 1);
    w.put(c.partition, PARTITION_BITS[mode]);
    if (mode == 4 || mode == 5) {
        w.put(c.rotation, 2);
    }
    if (mode == 4) {
        w.put(c.index_selection, 1);
    }
    uint8_t ch_order[4] = { 0, 1, 2, 3 };
    if (c.rotation != 0) {
        std::swap(ch_order[3], ch_order[c.rotation - 1]);
    }

    for (int i = 0; i < 4; ++i) {
        uint8_t ch = ch_order[i];
        uint8_t p = prec[ch];
        if (p == 0) continue;
        uint32_t drop = (ch == 0 && has_pbits) ? 1 : 0;
        for (size_t e = 0; e < 2 * subsets; ++e) {
            uint32_t field_val = st.ep[e / 2][2 * ch + e % 2] >> (8 - p);
            w.put(field_val >> drop, p - drop);
        }
    }
    if (mode == 1) {
        for (size_t row = 0; row < subsets; ++row) {
            w.put((st.ep[row][0] >> (8 - prec[0])) & 1, 1);
        }
    } else if (has_pbits) {
        for (size_t e = 0; e < 2 * subsets; ++e) {
            w.put((st.ep[e / 2][e % 2] >> (8 - prec[0])) & 1, 1);
        }
    }
    uint16_t anchors = 1;
    if (subsets == 2) {
        anchors = 1 | (1 << ANCHOR_2[c.partition & 63]);
    } else if (subsets == 3) {
        anchors = 1 | (1 << ANCHOR_3A[c.partition & 63]) | (1 << ANCHOR_3B[c.partition & 63]);
    }
    auto write_set = [&](size_t s) {
        uint32_t b = INDEX_BITS[mode][s];
        for (int p_idx = 0; p_idx < 16; ++p_idx) {
            uint32_t n = b - ((anchors >> p_idx) & 1);
            uint32_t val = st.ix[s][p_idx] >> (4 - b);
            w.put(val, n);
        }
    };
    if (mode == 4) {
        write_set(1);
        write_set(0);
    } else if (mode == 5) {
        write_set(0);
        write_set(1);
    } else {
        write_set(0);
    }
    w.write_to(out);
}

inline bool decode_bc7_blocks(const std::vector<uint16_t>& symbols, size_t num_blocks,
                              uint8_t* output, size_t output_capacity) {
    if (output_capacity < num_blocks * 16) return false;
    size_t target = num_blocks * 2;
    Bc7State hist[256] = {};

    for (size_t i = 0; i < num_blocks; ++i) {
        uint16_t l0_e = symbols[0 * target + 2 * i];
        uint16_t l0_o = symbols[0 * target + 2 * i + 1];
        uint16_t l1_e = symbols[1 * target + 2 * i];
        uint16_t l1_o = symbols[1 * target + 2 * i + 1];

        uint8_t d_byte = interleave(l0_e, l0_o);
        uint8_t x_byte = interleave(l1_e, l1_o);
        int32_t delta = unzigzag(d_byte);
        size_t er = static_cast<size_t>((x_byte + delta) & 0xFF);
        size_t xr = static_cast<size_t>(x_byte);

        uint16_t e15 = symbols[15 * target + 2 * i];
        uint16_t o15 = symbols[15 * target + 2 * i + 1];
        size_t code = ((o15 & 15) << 5) | (e15 & 31);
        if (code >= 512 || !BC7_CONTROL_TABLE[code].valid) {
            // The reference decoder emits a zero block for unmapped codes; for valid BC7 streams
            // all 285 mapped codes are legal. An unmapped code indicates stream corruption,
            // so fail fast per repository fail-fast policy (Refs #1139).
            return false;
        }
        const Bc7Control& c = BC7_CONTROL_TABLE[code];

        auto get_source = [&](size_t r, Bc7State& st) {
            if (r == 0) {
                st = {};
            } else if (r <= i) {
                st = hist[(i - r) & 255];
            } else {
                size_t dict_idx = r - i - 1;
                if (dict_idx < 255) {
                    const auto& entry = BC7_DICTIONARY[dict_idx];
                    st.subsets = entry.subsets;
                    std::memcpy(st.ep, entry.ep, sizeof(st.ep));
                    std::memcpy(st.ix, entry.ix, sizeof(st.ix));
                } else {
                    st = {};
                }
            }
        };

        Bc7State er_state, xr_state;
        get_source(er, er_state);
        get_source(xr, xr_state);

        Bc7State st = {};
        st.subsets = SUBSETS[c.mode];
        uint8_t prec[4];
        for (int ch = 0; ch < 4; ++ch) prec[ch] = CHANNEL_BITS[c.mode][ch];
        if (c.rotation != 0) {
            std::swap(prec[3], prec[c.rotation - 1]);
        } else if (c.mode == 0 || c.mode == 1 || c.mode == 3 || c.mode == 6 || c.mode == 7) {
            prec[0] += 1;
        }

        size_t common = (st.subsets < er_state.subsets) ? st.subsets : er_state.subsets;
        for (int ch = 0; ch < 4; ++ch) {
            uint8_t p = prec[ch];
            if (p == 0) continue;
            for (size_t row = 0; row < st.subsets; ++row) {
                size_t from = (row < common) ? row : 0;
                int lane = endpoint_lane(st.subsets, row, ch);
                for (int e = 0; e < 2; ++e) {
                    size_t col = 2 * ch + e;
                    uint8_t res = 0;
                    if (lane >= 0) {
                        res = residual(symbols[lane * target + 2 * i + e], p);
                    }
                    uint8_t base = er_state.ep[from][col] >> (8 - p);
                    st.ep[row][col] = ((base + res) & ((1 << p) - 1)) << (8 - p);
                }
            }
        }

        for (int set = 0; set < 2; ++set) {
            uint8_t b = INDEX_BITS[c.mode][set];
            if (b == 0) continue;
            for (int row = 0; row < 4; ++row) {
                int lane = INDEX_LANES[set][row];
                for (int half = 0; half < 2; ++half) {
                    uint16_t sym = symbols[lane * target + 2 * i + half];
                    uint16_t l, r;
                    index_pair(sym, row, l, r);
                    uint16_t pair[2] = { l, r };
                    for (int h = 0; h < 2; ++h) {
                        size_t p_idx = 4 * row + 2 * half + h;
                        uint8_t res = residual(pair[h], b);
                        uint8_t base = xr_state.ix[set][p_idx] >> (4 - b);
                        st.ix[set][p_idx] = ((base + res) & ((1 << b) - 1)) << (4 - b);
                    }
                }
            }
        }

        pack_bc7_block(c, st, output + i * 16);
        hist[i & 255] = st;
    }
    return true;
}

inline bool decode_bc1_blocks(const std::vector<uint16_t>& symbols, size_t target,
                              uint8_t* output, size_t output_capacity) {
    if (output_capacity < target * 8) return false;
    const uint8_t ENDPOINT_WIDTHS[6] = { 5, 6, 5, 5, 6, 5 };
    uint8_t hist[256][6] = {};
    for (size_t i = 0; i < target; ++i) {
        uint16_t l0_sym = symbols[0 * target + i];
        size_t v = l0_sym & 0xFF;
        size_t d = (v & 1) ? (255 - (v >> 1)) : (v >> 1);
        uint8_t pred[6] = {};
        if (d == 0) {
            // zero
        } else if (d <= i) {
            std::memcpy(pred, hist[(i - d) & 255], 6);
        } else {
            size_t slot = (static_cast<int>(i) - static_cast<int>(d)) & 255;
            std::memcpy(pred, BC1_WINDOW[slot], 6);
        }
        uint8_t e[6];
        for (int k = 0; k < 6; ++k) {
            uint16_t sym = symbols[(2 + k) * target + i];
            int32_t res = unzigzag(sym);
            uint32_t mask = (1u << ENDPOINT_WIDTHS[k]) - 1;
            e[k] = static_cast<uint8_t>((pred[k] + res) & mask);
        }
        std::memcpy(hist[i & 255], e, 6);
        uint16_t c0 = (static_cast<uint16_t>(e[0]) << 11) | (static_cast<uint16_t>(e[1]) << 5) | e[2];
        uint16_t c1 = (static_cast<uint16_t>(e[3]) << 11) | (static_cast<uint16_t>(e[4]) << 5) | e[5];
        uint32_t idx0 = symbols[8 * target + i] & 0xFF;
        uint32_t idx1 = symbols[9 * target + i] & 0xFF;
        uint32_t idx2 = symbols[10 * target + i] & 0xFF;
        uint32_t idx3 = symbols[11 * target + i] & 0xFF;
        uint32_t w = BC1_QUAD_MASKS[0][idx0] | BC1_QUAD_MASKS[1][idx1] |
                     BC1_QUAD_MASKS[2][idx2] | BC1_QUAD_MASKS[3][idx3];
        uint8_t* blk = output + i * 8;
        std::memcpy(blk + 0, &c0, 2);
        std::memcpy(blk + 2, &c1, 2);
        std::memcpy(blk + 4, &w, 4);
    }
    return true;
}

inline bool decode_bcpack(const unsigned char* input, size_t input_size, unsigned bc_format,
                          unsigned char* output, size_t output_size, size_t* produced) {
    if (!input || !output || !produced) return false;
    if (input_size == 0 || output_size == 0) return false;

    if (bc_format == 7) {
        if (output_size % 16 != 0) return false;
        size_t num_blocks = output_size / 16;
        size_t target_per_lane = num_blocks * 2;
        const uint8_t ab[16] = { 3, 3, 7, 7, 7, 7, 7, 6, 6, 4, 5, 7, 7, 7, 7, 4 };
        std::vector<uint16_t> symbols;
        if (!decode_lanes(input, input_size, ab, 16, target_per_lane, symbols)) {
            return false;
        }
        if (!decode_bc7_blocks(symbols, num_blocks, output, output_size)) {
            return false;
        }
        *produced = output_size;
        return true;
    } else if (bc_format == 1) {
        if (output_size % 8 != 0) return false;
        size_t num_blocks = output_size / 8;
        size_t target_per_lane = num_blocks;
        const uint8_t ab[12] = { 7, 7, 4, 5, 4, 4, 5, 4, 7, 7, 7, 7 };
        std::vector<uint16_t> symbols;
        if (!decode_lanes(input, input_size, ab, 12, target_per_lane, symbols)) {
            return false;
        }
        if (!decode_bc1_blocks(symbols, num_blocks, output, output_size)) {
            return false;
        }
        *produced = output_size;
        return true;
    }
    return false;
}

} // namespace x2w_bcpack
