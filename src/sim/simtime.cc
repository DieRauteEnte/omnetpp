//==========================================================================
//   SIMTIME.CC  - part of
//                     OMNeT++/OMNEST
//            Discrete System Simulation in C++
//
//==========================================================================

/*--------------------------------------------------------------*
  Copyright (C) 1992-2017 Andras Varga
  Copyright (C) 2006-2017 OpenSim Ltd.

  This file is distributed WITHOUT ANY WARRANTY. See the file
  `license' for details on this and other legal matters.
*--------------------------------------------------------------*/

#include <sstream>
#include <limits>
#include <cinttypes>  // PRId64
#include <mutex>
#include "common/opp_ctype.h"
#include "common/unitconversion.h"
#include "common/stringutil.h"
#include "omnetpp/simtime.h"
#include "omnetpp/cexception.h"
#include "omnetpp/cpar.h"
#include "omnetpp/onstartup.h"
#include "omnetpp/csimulation.h"
#include "omnetpp/cconfiguration.h"
#include "omnetpp/cconfigoption.h"
#include "omnetpp/globals.h"
#include "omnetpp/regmacros.h"

using namespace omnetpp::common;

namespace omnetpp {

Register_GlobalConfigOption(CFGID_SIMTIME_SCALE, "simtime-scale", CFG_INT, "-12", "DEPRECATED in favor of simtime-resolution. Sets the scale exponent, and thus the resolution of time for the 64-bit fixed-point simulation time representation. Accepted values are -18..0; for example, -6 selects microsecond resolution. -12 means picosecond resolution, with a maximum simtime of ~110 days. Note: Once this option is set at runtime in the simulation library, it cannot be changed later.");
Register_GlobalConfigOption(CFGID_SIMTIME_RESOLUTION, "simtime-resolution", CFG_CUSTOM, "ps", "Sets the resolution for the 64-bit fixed-point simulation time representation. Accepted values are: second-or-smaller time units (`s`, `ms`, `us`, `ns`, `ps`, `fs` or as), power-of-ten multiples of such units (e.g. 100ms), and base-10 scale exponents in the -18..0 range. The maximum representable simulation time depends on the resolution. The default is picosecond resolution, which offers a range of ~110 days. Note: Once this option is set at runtime in the simulation library, it cannot be changed later.");


int SimTime::scaleexp = SimTime::SCALEEXP_UNINITIALIZED;
int64_t SimTime::dscale;
double SimTime::fscale;
double SimTime::invfscale;
int64_t SimTime::maxseconds;
bool SimTime::checkmul = true;

const SimTime SimTime::ZERO;

#define MAX_POWER_OF_TEN  18
static int64_t powersOfTen[MAX_POWER_OF_TEN+1];

static const char *unitNames[] = { "s", "ms", "us", "ns", "ps", "fs", "as" };

static void fillPowersOfTen()
{
    int64_t power = 1;
    for (int i = 0; i <= MAX_POWER_OF_TEN; i++) {
        if (i != 0)
            power *= 10;
        powersOfTen[i] = power;
    }
}

static struct PowersOfTenInitializer {
    PowersOfTenInitializer() {fillPowersOfTen();}
} dummy;


inline int64_t exp10(int exponent)
{
    if (exponent < 0 || exponent > MAX_POWER_OF_TEN)
        return -1;  // error
    return powersOfTen[exponent];
}

void SimTime::configure(cConfiguration *cfg)
{
    bool hasSimtimeResolution = cfg->getConfigValue(CFGID_SIMTIME_RESOLUTION->getName()) != nullptr;
    bool hasSimtimeScale = cfg->getConfigValue(CFGID_SIMTIME_SCALE->getName()) != nullptr;
    int exp;
    if (hasSimtimeResolution || !hasSimtimeScale)
        exp = parseSimtimeResolution(cfg->getAsCustom(CFGID_SIMTIME_RESOLUTION)); // new
    else
        exp = (int)cfg->getAsInt(CFGID_SIMTIME_SCALE); // legacy

    SimTime::setScaleExp(exp);

    if (hasSimtimeScale)
        cSimulation::getActiveEnvir()->printfmsg(
                "Warning: Obsolete config option %s= found, please use the improved %s= instead "
                "(it allows values like \"us\" or \"100ps\" in addition to base-10 scale exponents)",
                CFGID_SIMTIME_SCALE->getName(), CFGID_SIMTIME_RESOLUTION->getName());
}

int SimTime::parseSimtimeResolution(const char *resolution)
{
    try {
        // Three possibilities: <unit-only>, <number-with-unit>, <number-only>
        const double INVALID_EXP = 1e100;
        double exp = INVALID_EXP;
        if (opp_isalpha(resolution[0])) {
            // try as <unit-only>, e.g. "ms"
            double f = UnitConversion::getConversionFactor(resolution, "s");
            if (f == 0)
                throw std::runtime_error("Wrong unit");
            exp = log10(f);
            ASSERT(exp == floor(exp));
        }
        else {
            // try as <number-only>: this will be an exponent, e.g. -12
            try { exp = opp_atol(resolution); } catch (std::exception& e) {}

            // try as <number-with-unit>, e.g. "100ps"
            if (exp == INVALID_EXP) {
                double f = UnitConversion::parseQuantity(resolution, "s");
                exp = log10(f);
                if (exp != floor(exp))
                    throw std::runtime_error("Not power of 10");
            }
        }
        return (int)exp;
    }
    catch (std::exception& e) {
        throw cRuntimeError(
                "Invalid value \"%s\" for configuration option simtime-resolution: it must be "
                "a valid second-or-smaller time unit (s, ms, us, ns, ps, fs or as), "
                "a power-of-ten multiple of such unit (e.g. 100ms), or a base-10 scale "
                "exponent in the -18..0 range. (Details: %s)", resolution, e.what());
    }
}

static std::mutex scaleExpMutex;

void SimTime::setScaleExp(int e)
{
    if (e < -18 || e > 0)
        throw cRuntimeError("SimTime scale exponent %d is out of accepted range -18..0", e);

    std::lock_guard<std::mutex> guard(scaleExpMutex);
    if (e == scaleexp)
        return;
    if (scaleexp != SCALEEXP_UNINITIALIZED)
        throw cRuntimeError("SimTime scale exponent (i.e. simulation time resolution) cannot be changed once it has been set up (currently %d, requested %d)", scaleexp, e);

    scaleexp = e;
    dscale = exp10(-scaleexp);
    fscale = (double)dscale;
    invfscale = 1.0 / fscale;
    maxseconds = INT64_MAX / dscale;
}

static std::string range()
{
    return std::string("(-") + SimTime::getMaxTime().str() + "," + SimTime::getMaxTime().str() + ")";
}

void SimTime::initError(double d)
{
    throw cRuntimeError("Attempting to initialize a simtime_t variable with a nonzero value (%g) "
                        "before the scale exponent has been set; if such early initialization is needed, "
                        "you may want to use double or const_simtime_t instead of simtime_t", d);
}

void SimTime::rangeErrorInt64(double i64)
{
    throw cRuntimeError("Cannot convert %g to simtime_t: Out of range %s, allowed by scale exponent %d",
            i64*invfscale, range().c_str(), scaleexp);
}

void SimTime::rangeErrorSeconds(int64_t sec)
{
    throw cRuntimeError("Cannot convert % " PRId64 "D to simtime_t: Out of range %s, allowed by scale exponent %d",
            sec, range().c_str(), scaleexp);
}

void SimTime::overflowAdding(const SimTime& x)
{
    t -= x.t;  // restore original value
    throw cRuntimeError("simtime_t overflow adding %s to %s: Result is out of range %s, allowed by scale exponent %d",
            x.str().c_str(), str().c_str(), range().c_str(), scaleexp);
}

void SimTime::overflowSubtracting(const SimTime& x)
{
    t += x.t;  // restore original value
    throw cRuntimeError("simtime_t overflow subtracting %s from %s: Result is out of range %s, allowed by scale exponent %d",
            x.str().c_str(), str().c_str(), range().c_str(), scaleexp);
}

void SimTime::overflowNegating()
{
    throw cRuntimeError("Cannot negate simtime_t %s: It is internally represented with INT64_MIN "
            "that has no positive equivalent (try decreasing precision)", str().c_str());
}

SimTime::SimTime(int64_t value, SimTimeUnit unit)
{
    int exponent = unit;
    if (scaleexp == SCALEEXP_UNINITIALIZED)
        throw cRuntimeError("Attempting to initialize a simtime_t variable with a nonzero value (%" PRId64 "*10^%ds) "
                            "before the scale exponent has been set; if such early initialization is needed, "
                            "you may want to use double or const_simtime_t instead of simtime_t", value, exponent);

    t = value;
    int expdiff = exponent - scaleexp;
    if (expdiff < 0) {
        int64_t mul = exp10(-expdiff);
        int64_t tmp = t / mul;
        if (mul == -1 || tmp * mul != t)
            throw cRuntimeError("simtime_t: %" PRId64 "*10^%d cannot be represented precisely using the current scale exponent %d, "
                    "increase resolution by configuring a smaller scale exponent or use 'double' conversion",
                    value, exponent, scaleexp);
        t = tmp;
    }
    else if (expdiff > 0) {
        int64_t mul = exp10(expdiff);
        t *= mul;
        if (mul == -1 || t / mul != value)
            throw cRuntimeError("simtime_t overflow: Cannot represent %" PRId64 "*10^%d, out of range %s allowed by scale exponent %d",
                    value, exponent, range().c_str(), scaleexp);
    }
}

void SimTime::checkedMul(int64_t x)
{
    int64_t tmp = t * x;
    if (x == 0 || tmp / x == t) {
        t = tmp;
        return;
    }

    throw cRuntimeError("simtime_t overflow multiplying %s by %" PRId64 ": Result is out of range %s, allowed by scale exponent %d",
        str().c_str(), x, range().c_str(), scaleexp);
}

int64_t SimTime::inUnit(SimTimeUnit unit) const
{
    int64_t x = t;
    int exponent = unit;
    int expdiff = exponent - scaleexp;
    if (expdiff > 0) {
        int64_t mul = exp10(expdiff);
        x = (mul == -1) ? 0 : x / mul;
    }
    else if (expdiff < 0) {
        int64_t mul = exp10(-expdiff);
        if (mul == -1 || (x < 0 ? -x : x) >= INT64_MAX / mul)
            throw cRuntimeError("SimTime::inUnit(): Overflow: Cannot represent %s in units of 10^%ds", str().c_str(), exponent);
        x *= mul;
    }
    return x;
}

void SimTime::split(SimTimeUnit unit, int64_t& outValue, SimTime& outRemainder) const
{
    outValue = inUnit(unit);
    outRemainder = *this - SimTime(outValue, unit);
}

SimTime& SimTime::operator=(const cPar& p)
{
    switch (p.getType()) {
        case cPar::INT: return operator=(p.intValue());
        case cPar::DOUBLE: return operator=(p.doubleValue());
        default: throw cRuntimeError(&p, "Cannot convert non-numeric parameter to simtime_t");
    }
}

const SimTime& SimTime::operator*=(const cPar& p)
{
    switch (p.getType()) {
        case cPar::INT: return operator*=(p.intValue());
        case cPar::DOUBLE: return operator*=(p.doubleValue());
        default: throw cRuntimeError(&p, "Cannot convert non-numeric parameter to simtime_t");
    }
}

const SimTime& SimTime::operator/=(const cPar& p)
{
    switch (p.getType()) {
        case cPar::INT: return operator/=(p.intValue());
        case cPar::DOUBLE: return operator/=(p.doubleValue());
        default: throw cRuntimeError(&p, "Cannot convert non-numeric parameter to simtime_t");
    }
}

static int64_t gcd(int64_t a, int64_t b)
{
    while (b != 0) {
        int64_t tmp = a % b;
        a = b;
        b = tmp;
    }
    return a;
}

double operator/(long long x, const SimTime& y)
{
    // Try to compute it precisely, as x * 10^scaleexp / y.raw().
    //
    // Pitfall: (x * 10^scaleexp) might overflow. We can mitigate that trying
    // to simplify the fraction by dividing both numerator and denominator by their GCD.
    // If numerator still overflows, perform computation in double.
    // If denominator becomes 1, return numerator cast to double.
    // Otherwise perform division using double arithmetic, and return the result.

    int64_t num1 = x;
    int64_t num2 = powersOfTen[-SimTime::scaleexp];
    int64_t denom = y.t;

    int64_t num = num1*num2;
    if (num / num2 != num1) { // overflow, try simplification
        if (denom == 0)
            return (double)num1 * (double)num2 / double(denom);
        int64_t gcd1 = gcd(num1, denom);
        num1 /= gcd1; denom /= gcd1;

        int64_t gcd2 = gcd(num2, denom);
        num2 /= gcd2; denom /= gcd2;

        num = num1*num2;
        if (num / num2 != num1) // still overflow, fall back to double
            return (double)num1 * (double)num2 / double(denom);
    }

    return (denom == 1) ? (double)num : (double)num / (double)denom;
}

double operator/(unsigned long long x, const SimTime& y)
{
    if (x <= std::numeric_limits<long long>::max())
        return operator/((long long)x, y);
    else if ((x&1) == 0 || x+1 == 0)
        return 2.0 * operator/((long long)(x/2), y);
    else
        return 2.0 * operator/((long long)(x/2+1), y); // round up x/2 (unless it's maxint, see x+1==0 condition above)
}

double operator/(const cPar& p, const SimTime& x)
{
    switch (p.getType()) {
        case cPar::INT: return p.intValue() / x;
        case cPar::DOUBLE: return p.doubleValue() / x;
        default: throw cRuntimeError(&p, "Cannot convert non-numeric parameter to simtime_t");
    }
}

char *SimTime::ttoa(char *buf, int64_t t, int scaleexp, char *& endp)
{
    return opp_ttoa(buf, t, scaleexp, endp);
}

std::string SimTime::ustr() const
{
    if (t == 0)
        return "0s";

    // compute ~abs(t)
    int64_t tt = t;
    if (tt < 0) {
        tt = -tt;
        if (tt < 0)
            tt = -(tt+1);
    }

    // determine unit to print in (seconds and smaller units are considered)
    int unitExp = 0;
    while (unitExp > scaleexp && tt < powersOfTen[-scaleexp+unitExp])
        unitExp -= 3;
    return ustr((SimTimeUnit)unitExp);
}

std::string SimTime::ustr(SimTimeUnit unit) const
{
    int unitExp = (int)unit;
    char buf[80];
    char *endp;
    const char *result = opp_ttoa(buf, t, scaleexp-unitExp, endp);
    strcpy(endp, unitNames[-unitExp/3]);
    return result;
}


std::string SimTime::format(int prec, const char *decimalSep, const char *digitSep, bool addUnits, const char *beforeUnit, const char *afterUnit) const
{
    ASSERT(scaleexp <= 0 && scaleexp >= -18);

    if (prec > 0 || prec < -18)
        throw cRuntimeError("SimTime::format(): prec=%d out of range, must be in 0..-18", prec);

    if (digitSep && !*digitSep) digitSep = nullptr;
    if (!beforeUnit) beforeUnit = "";
    if (!afterUnit) afterUnit = "";

    std::stringstream out;
    if (t < 0)
        out << "-";

    char digits[32];
    snprintf(digits, sizeof(digits), "%" PRId64, t<0 ? -t : t);
    int numDigits = strlen(digits);

    int startDecimal = scaleexp + numDigits - 1;
    int endDecimal = prec;

    if (startDecimal < 0)
        startDecimal = 0;  // always print seconds
    if (endDecimal > 0)
        endDecimal = 0;  // always print seconds
    if ((endDecimal % 3) != 0 && (addUnits || digitSep))
        endDecimal = 3*((endDecimal-2)/3); // make it multiple of 3


    for (int decimalPlace = startDecimal; decimalPlace >= endDecimal; decimalPlace--) {
        int index = (scaleexp + numDigits - 1) - decimalPlace;
        out << ((index < 0 || index >= numDigits) ? '0' : digits[index]);
        if (decimalPlace % 3 == 0) {
            if (addUnits && decimalPlace <= 0 && decimalPlace >= -18) {
                out << beforeUnit << unitNames[-decimalPlace/3] << afterUnit;
            }
            else if (decimalPlace == 0) {
                if (endDecimal < 0)
                    out << decimalSep;
            }
            else if (digitSep && decimalPlace != endDecimal) {
                out << digitSep;
            }
        }
    }

    return out.str();
}

const SimTime SimTime::parse(const char *s)
{
    try {
        // Note: UnitConversion calculates in double, so we may lose precision during conversion
        std::string unit;
        double d = UnitConversion::parseQuantity(s, unit);  // "unit" is OUT parameter
        return unit.empty() ? d : UnitConversion::convertUnit(d, unit.c_str(), "s");
    }
    catch (std::exception& e) {
        throw cRuntimeError("Cannot convert string \"%s\" to SimTime: %s", s, e.what());
    }
}

const SimTime SimTime::parse(const char *s, const char *& endp)
{
    // find end of the simtime literal in the string
    endp = s;
    while (opp_isspace(*endp))
        endp++;
    if (!*endp)
        {endp = s; return 0;} // it was just space

    while (opp_isalnum(*endp) || opp_isspace(*endp) || *endp == '+' || *endp == '-' || *endp == '.')
        endp++;

    // delegate to the other parse() method
    return parse(std::string(s, endp - s).c_str());
}

}  // namespace omnetpp

