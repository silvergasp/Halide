#include "Halide.h"
#include "fuzz_helpers.h"
#include <array>
#include <fuzzer/FuzzedDataProvider.h>
#include <random>
#include <stdio.h>
#include <time.h>

namespace {

using std::map;
using std::string;
using namespace Halide;
using namespace Halide::Internal;

#define internal_assert _halide_user_assert

const int fuzz_var_count = 5;

std::mt19937 rng(0);

Type fuzz_types[] = {UInt(1), UInt(8), UInt(16), UInt(32), Int(8), Int(16), Int(32)};
const int fuzz_type_count = sizeof(fuzz_types) / sizeof(fuzz_types[0]);

std::string fuzz_var(int i) {
    return std::string(1, 'a' + i);
}

// This is modified for each round.
static Type global_var_type = Int(32);

Expr random_var(FuzzedDataProvider &fdp) {
    int fuzz_count = fdp.ConsumeIntegralInRange(0, fuzz_var_count - 1);
    return Variable::make(global_var_type, fuzz_var(fuzz_count));
}

Type random_type(FuzzedDataProvider &fdp, int width) {
    Type T = fdp.PickValueInArray(fuzz_types);

    if (width > 1) {
        T = T.with_lanes(width);
    }
    return T;
}

int get_random_divisor(FuzzedDataProvider &fdp, Type t) {
    std::vector<int> divisors = {t.lanes()};
    for (int dd = 2; dd < t.lanes(); dd++) {
        if (t.lanes() % dd == 0) {
            divisors.push_back(dd);
        }
    }

    return pick_value_in_vector(fdp, divisors);
}

Expr random_leaf(FuzzedDataProvider &fdp, Type T, bool overflow_undef = false, bool imm_only = false) {
    if (T.is_int() && T.bits() == 32) {
        overflow_undef = true;
    }
    if (T.is_scalar()) {
        if (!imm_only && fdp.ConsumeBool()) {
            auto v1 = random_var(fdp);
            return cast(T, v1);
        } else if (overflow_undef) {
            // For Int(32), we don't care about correctness during
            // overflow, so just use numbers that are unlikely to
            // overflow.
            return cast(T, fdp.ConsumeIntegralInRange<int>(-128, 127));
        } else {
            return cast(T, fdp.ConsumeIntegral<int>());
        }
    } else {
        int lanes = get_random_divisor(fdp, T);
        if (fdp.ConsumeBool()) {
            auto e1 = random_leaf(fdp, T.with_lanes(T.lanes() / lanes), overflow_undef);
            auto e2 = random_leaf(fdp, T.with_lanes(T.lanes() / lanes), overflow_undef);
            return Ramp::make(e1, e2, lanes);
        } else {
            auto e1 = random_leaf(fdp, T.with_lanes(T.lanes() / lanes), overflow_undef);
            return Broadcast::make(e1, lanes);
        }
    }
}

Expr random_expr(FuzzedDataProvider &fdp, Type T, int depth, bool overflow_undef = false);

Expr random_condition(FuzzedDataProvider &fdp, Type T, int depth, bool maybe_scalar) {
    typedef Expr (*make_bin_op_fn)(Expr, Expr);
    static make_bin_op_fn make_bin_op[] = {
        EQ::make,
        NE::make,
        LT::make,
        LE::make,
        GT::make,
        GE::make,
    };

    if (maybe_scalar && fdp.ConsumeBool()) {
        T = T.element_of();
    }

    Expr a = random_expr(fdp, T, depth);
    Expr b = random_expr(fdp, T, depth);
    return fdp.PickValueInArray(make_bin_op)(a, b);
}

Expr random_expr(FuzzedDataProvider &fdp, Type T, int depth, bool overflow_undef) {

    if (T.is_int() && T.bits() == 32) {
        overflow_undef = true;
    }
    if (depth-- <= 0) {
        return random_leaf(fdp, T, overflow_undef);
    }

    std::function<Expr()> operations[] = {
        [&]() {
            return random_leaf(fdp, T);
        },
        [&]() {
            auto c = random_condition(fdp, T, depth, true);
            auto e1 = random_expr(fdp, T, depth, overflow_undef);
            auto e2 = random_expr(fdp, T, depth, overflow_undef);
            return Select::make(c, e1, e2);
        },
        [&]() {
            if (T.lanes() != 1) {
                int lanes = get_random_divisor(fdp, T);
                auto e1 = random_expr(fdp, T.with_lanes(T.lanes() / lanes), depth, overflow_undef);
                return Broadcast::make(e1, lanes);
            }
            // If we got here, try again.
            return random_expr(fdp, T, depth, overflow_undef);
        },
        [&]() {
            if (T.lanes() != 1) {
                int lanes = get_random_divisor(fdp, T);
                auto e1 = random_expr(fdp, T.with_lanes(T.lanes() / lanes), depth, overflow_undef);
                auto e2 = random_expr(fdp, T.with_lanes(T.lanes() / lanes), depth, overflow_undef);
                return Ramp::make(e1, e2, lanes);
            }
            // If we got here, try again.
            return random_expr(fdp, T, depth, overflow_undef);
        },
        [&]() {
            if (T.is_bool()) {
                auto e1 = random_expr(fdp, T, depth);
                return Not::make(e1);
            }
            // If we got here, try again.
            return random_expr(fdp, T, depth, overflow_undef);
        },
        [&]() {
            if (T.is_bool()) {
                return random_condition(fdp, random_type(fdp, T.lanes()), depth, false);
            }
            // If we got here, try again.
            return random_expr(fdp, T, depth, overflow_undef);
        },
        [&]() {
            Type subT;
            do {
                subT = random_type(fdp, T.lanes());
            } while (subT == T || (subT.is_int() && subT.bits() == 32));
            auto e1 = random_expr(fdp, subT, depth, overflow_undef);
            return Cast::make(T, e1);
        },
        [&]() {
            typedef Expr (*make_bin_op_fn)(Expr, Expr);
            static make_bin_op_fn make_bin_op[] = {
                // Arithmetic operations.
                Add::make,
                Sub::make,
                Mul::make,
                Min::make,
                Max::make,
                Div::make,
                Mod::make,
                // Binary operations.
                And::make,
                Or::make,
            };
            make_bin_op_fn maker = fdp.PickValueInArray(make_bin_op);
            Expr a = random_expr(fdp, T, depth, overflow_undef);
            Expr b = random_expr(fdp, T, depth, overflow_undef);
            return maker(a, b);
        },
    };
    return fdp.PickValueInArray(operations)();
}

// These are here to enable copy of failed output expressions and pasting them into the test for debugging.
Expr ramp(Expr b, Expr s, int w) {
    return Ramp::make(b, s, w);
}
Expr x1(Expr x) {
    return Broadcast::make(x, 2);
}
Expr x2(Expr x) {
    return Broadcast::make(x, 2);
}
Expr x3(Expr x) {
    return Broadcast::make(x, 3);
}
Expr x4(Expr x) {
    return Broadcast::make(x, 2);
}
Expr x6(Expr x) {
    return Broadcast::make(x, 6);
}
Expr x8(Expr x) {
    return Broadcast::make(x, 8);
}
Expr uint1(Expr x) {
    return Cast::make(UInt(1), x);
}
Expr uint8(Expr x) {
    return Cast::make(UInt(8), x);
}
Expr uint16(Expr x) {
    return Cast::make(UInt(16), x);
}
Expr uint32(Expr x) {
    return Cast::make(UInt(32), x);
}
Expr int8(Expr x) {
    return Cast::make(Int(8), x);
}
Expr int16(Expr x) {
    return Cast::make(Int(16), x);
}
Expr int32(Expr x) {
    return Cast::make(Int(32), x);
}
Expr uint1x2(Expr x) {
    return Cast::make(UInt(1).with_lanes(2), x);
}
Expr uint8x2(Expr x) {
    return Cast::make(UInt(8).with_lanes(2), x);
}
Expr uint16x2(Expr x) {
    return Cast::make(UInt(16).with_lanes(2), x);
}
Expr uint32x2(Expr x) {
    return Cast::make(UInt(32).with_lanes(2), x);
}
Expr uint32x3(Expr x) {
    return Cast::make(UInt(32).with_lanes(3), x);
}
Expr int8x2(Expr x) {
    return Cast::make(Int(8).with_lanes(2), x);
}
Expr int16x2(Expr x) {
    return Cast::make(Int(16).with_lanes(2), x);
}
Expr int16x3(Expr x) {
    return Cast::make(Int(16).with_lanes(3), x);
}
Expr int32x2(Expr x) {
    return Cast::make(Int(32).with_lanes(2), x);
}

Expr a(Variable::make(global_var_type, fuzz_var(0)));
Expr b(Variable::make(global_var_type, fuzz_var(1)));
Expr c(Variable::make(global_var_type, fuzz_var(2)));
Expr d(Variable::make(global_var_type, fuzz_var(3)));
Expr e(Variable::make(global_var_type, fuzz_var(4)));

std::ostream &operator<<(std::ostream &stream, const Interval &interval) {
    stream << "[" << interval.min << ", " << interval.max << "]";
    return stream;
}

Interval random_interval(FuzzedDataProvider &fdp, Type T) {
    Interval interval;

    int min_value = -128;
    int max_value = 128;

    Type t = T.element_of();
    if ((t.is_uint() || (t.is_int() && t.bits() <= 16))) {
        Expr t_min = t.min();
        Expr t_max = t.max();
        if (auto ptr = as_const_int(t_min)) {
            min_value = *ptr;
        } else if (auto ptr = as_const_uint(t_min)) {
            min_value = *ptr;
        } else {
            std::cerr << "random_interval failed to find min of: " << T << "\n";
        }
        if (auto ptr = as_const_int(t_max)) {
            max_value = *ptr;
        } else if (auto ptr = as_const_uint(t_max)) {
            // can't represent all uint32_t in int type
            if (*ptr <= 128) {
                max_value = *ptr;
            }
        } else {
            std::cerr << "random_interval failed to find max of: " << T << "\n";
        }
    }

    // Try to get rid of very large values that might overflow.
    min_value = std::max(min_value, -128);
    max_value = std::min(max_value, 128);

    // change the min_value for the calculation of max
    min_value = fdp.ConsumeIntegralInRange<int>(min_value, max_value);
    interval.min = cast(T, min_value);

    max_value = fdp.ConsumeIntegralInRange<int>(min_value, max_value);
    interval.max = cast(T, max_value);

    if (min_value > max_value || (interval.is_bounded() && can_prove(interval.min > interval.max))) {
        std::cerr << "random_interval failed: ";
        std::cerr << min_value << " > " << max_value << "\n";
        std::cerr << interval.min << " > " << interval.max << "\n";
        std::cerr << interval << "\n";
        internal_assert(false) << "random_interval failed\n";
    }

    return interval;
}

int sample_interval(FuzzedDataProvider &fdp, const Interval &interval) {
    // Values chosen so intervals don't repeatedly produce signed_overflow when simplified.
    int min_value = -128;
    int max_value = 128;

    if (interval.has_lower_bound()) {
        if (auto ptr = as_const_int(interval.min)) {
            min_value = *ptr;
        } else if (auto ptr = as_const_uint(interval.min)) {
            min_value = *ptr;
        } else {
            internal_assert(false) << "sample_interval (min) failed: " << interval.min << "\n";
        }
    }

    if (interval.has_upper_bound()) {
        if (auto ptr = as_const_int(interval.max)) {
            max_value = *ptr;
        } else if (auto ptr = as_const_uint(interval.max)) {
            max_value = *ptr;
        } else {
            internal_assert(false) << "sample_interval (max) failed: " << interval.max << "\n";
        }
    }

    return fdp.ConsumeIntegralInRange<int>(min_value, max_value);
}

bool test_bounds(Expr test, const Interval &interval, Type T, const map<string, Expr> &vars) {
    for (int j = 0; j < T.lanes(); j++) {
        Expr a_j = test;
        if (T.lanes() != 1) {
            a_j = extract_lane(test, j);
        }

        Expr a_j_v = simplify(substitute(vars, a_j));

        if (!is_const(a_j_v)) {
            // Probably overflow, abort.
            continue;
        }

        // This fuzzer only looks for constant bounds, otherwise it's probably overflow.
        if (interval.has_upper_bound()) {
            if (!can_prove(a_j_v <= interval.max)) {
                std::cerr << "can't prove upper bound: " << (a_j_v <= interval.max) << "\n";
                for (auto v = vars.begin(); v != vars.end(); v++) {
                    std::cerr << v->first << " = " << v->second << "\n";
                }

                std::cerr << test << "\n";
                std::cerr << interval << "\n";
                std::cerr << "In vector lane " << j << ":\n";
                std::cerr << a_j << " -> " << a_j_v << "\n";
                return false;
            }
        }

        if (interval.has_lower_bound()) {
            if (!can_prove(a_j_v >= interval.min)) {
                std::cerr << "can't prove lower bound: " << (a_j_v >= interval.min) << "\n";
                std::cerr << "Expr: " << test << "\n";
                std::cerr << "Interval: " << interval << "\n";

                for (auto v = vars.begin(); v != vars.end(); v++) {
                    std::cerr << v->first << " = " << v->second << "\n";
                }

                std::cerr << "In vector lane " << j << ":\n";
                std::cerr << a_j << " -> " << a_j_v << "\n";
                return false;
            }
        }
    }
    return true;
}

bool test_expression_bounds(FuzzedDataProvider &fdp, Expr test, int trials, int samples_per_trial) {

    map<string, Expr> vars;
    for (int i = 0; i < fuzz_var_count; i++) {
        vars[fuzz_var(i)] = Expr();
    }

    for (int i = 0; i < trials; i++) {
        Scope<Interval> scope;

        for (auto v = vars.begin(); v != vars.end(); v++) {
            // This type is used because the variables will be this type for a given round.
            Interval interval = random_interval(fdp, global_var_type);
            scope.push(v->first, interval);
        }

        Interval interval = bounds_of_expr_in_scope(test, scope);
        interval.min = simplify(interval.min);
        interval.max = simplify(interval.max);

        if (!(interval.has_upper_bound() || interval.has_lower_bound())) {
            // For now, return. Assumes that no other combo
            // will produce a bounded interval (not necessarily true).
            // This is to shorten the amount of output from this test.
            return true;  // any result is allowed
        }

        if ((interval.has_upper_bound() && is_signed_integer_overflow(interval.max)) ||
            (interval.has_lower_bound() && is_signed_integer_overflow(interval.min))) {
            // Quit for now, assume other intervals will produce the same results.
            return true;
        }

        if (!is_const(interval.min) || !is_const(interval.max)) {
            // Likely signed_integer_overflow, give up now.
            return true;
        }

        for (int j = 0; j < samples_per_trial; j++) {
            for (std::map<string, Expr>::iterator v = vars.begin(); v != vars.end(); v++) {
                Interval interval = scope.get(v->first);
                v->second = cast(global_var_type, sample_interval(fdp, interval));
            }

            if (!test_bounds(test, interval, test.type(), vars)) {
                std::cerr << "scope {"
                          << "\n";
                for (auto v = vars.begin(); v != vars.end(); v++) {
                    std::cerr << "\t" << v->first << " : " << scope.get(v->first) << "\n";
                }
                std::cerr << "}"
                          << "\n";
                return false;
            }
        }
    }
    return true;
}

}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    FuzzedDataProvider fdp(data, size);

    // Number of random expressions to test.
    const int count = 100;
    // Depth of the randomly generated expression trees.
    const int depth = 3;
    // Number of trials to test the generated expressions for.
    const int trials = 10;
    // Number of samples of the intervals per trial to test.
    const int samples = 10;

    std::array<int, 6> vector_widths = {1, 2, 3, 4, 6, 8};
    for (int n = 0; n < count; n++) {
        // int width = 1;
        int width = fdp.PickValueInArray(vector_widths);
        // This is the type that will be the innermost (leaf) value type.
        Type expr_type = random_type(fdp, width);
        Type var_type = random_type(fdp, 1);
        global_var_type = var_type;
        // Generate a random expr...
        Expr test = random_expr(fdp, expr_type, depth);
        if (!test_expression_bounds(fdp, test, trials, samples)) {
            return 1;
        }
    }

    std::cout << "Success!\n";
    return 0;
}
