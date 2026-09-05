/*
 * The calculator's modes, and the tables the converter works from.
 *
 * Separate from recon_calc.c because the unit tables are data and long, and a
 * file where the interesting hundred lines are buried under four hundred rows
 * of conversion factors is a file nobody reads the interesting part of.
 *
 * --- On the factors ---
 *
 * Every unit is expressed as "how many of the base unit is one of these", so
 * converting is one multiply and one divide and there is no table of pairs.
 * A table of pairs for fifteen units is two hundred and ten numbers, of which
 * about two hundred are somebody's arithmetic rather than a published value.
 *
 * The values are exact where an exact value exists. An inch is exactly
 * 0.0254 metres and a pound is exactly 0.45359237 kilograms, both by
 * international agreement in 1959; writing 0.4536 instead would be inventing
 * a disagreement with the rest of the world at the fourth decimal place.
 */

#ifndef RECON_CALC_MODES_H
#define RECON_CALC_MODES_H

#include <stdbool.h>

enum calc_mode {
    CALC_STANDARD,
    CALC_SCIENTIFIC,
    CALC_PROGRAMMER,
    CALC_DATE,
    CALC_CONVERT,
    CALC_MODE_COUNT,
};

static const char *const CALC_MODE_NAMES[CALC_MODE_COUNT] = {
    "Standard", "Scientific", "Programmer", "Date", "Convert",
};

/* --- Units --- */

struct calc_unit {
    const char *name;
    /* One of these, in base units. Unused for temperature, which needs an
     * offset as well and is converted by hand. */
    double factor;
};

struct calc_category {
    const char *name;
    const char *base;        /* what the factors are relative to */
    const struct calc_unit *units;
    int count;
    /* True for the one family that is not a simple ratio. Celsius to
     * Fahrenheit has an offset, so a factor alone cannot express it, and
     * pretending otherwise gets 0C wrong by 32 degrees. */
    bool has_offset;
};

static const struct calc_unit CALC_LENGTH[] = {
    { "millimetre", 0.001 }, { "centimetre", 0.01 }, { "metre", 1.0 },
    { "kilometre", 1000.0 }, { "inch", 0.0254 }, { "foot", 0.3048 },
    { "yard", 0.9144 }, { "mile", 1609.344 }, { "nautical mile", 1852.0 },
};

static const struct calc_unit CALC_MASS[] = {
    { "milligram", 0.000001 }, { "gram", 0.001 }, { "kilogram", 1.0 },
    { "tonne", 1000.0 }, { "ounce", 0.028349523125 },
    { "pound", 0.45359237 }, { "stone", 6.35029318 },
    { "US ton", 907.18474 }, { "UK ton", 1016.0469088 },
};

static const struct calc_unit CALC_TEMPERATURE[] = {
    { "Celsius", 0.0 }, { "Fahrenheit", 0.0 }, { "Kelvin", 0.0 },
};

static const struct calc_unit CALC_AREA[] = {
    { "square millimetre", 0.000001 }, { "square centimetre", 0.0001 },
    { "square metre", 1.0 }, { "hectare", 10000.0 },
    { "square kilometre", 1000000.0 }, { "square inch", 0.00064516 },
    { "square foot", 0.09290304 }, { "square yard", 0.83612736 },
    { "acre", 4046.8564224 }, { "square mile", 2589988.110336 },
};

static const struct calc_unit CALC_VOLUME[] = {
    { "millilitre", 0.001 }, { "litre", 1.0 }, { "cubic metre", 1000.0 },
    { "US fluid ounce", 0.0295735295625 }, { "US cup", 0.2365882365 },
    { "US pint", 0.473176473 }, { "US quart", 0.946352946 },
    { "US gallon", 3.785411784 }, { "UK pint", 0.56826125 },
    { "UK gallon", 4.54609 },
};

static const struct calc_unit CALC_SPEED[] = {
    { "metre per second", 1.0 }, { "kilometre per hour", 0.2777777777777778 },
    { "mile per hour", 0.44704 }, { "knot", 0.5144444444444445 },
    { "foot per second", 0.3048 },
};

static const struct calc_unit CALC_TIME[] = {
    { "millisecond", 0.001 }, { "second", 1.0 }, { "minute", 60.0 },
    { "hour", 3600.0 }, { "day", 86400.0 }, { "week", 604800.0 },
    /* The Julian year, 365.25 days, which is what "a year" means when it is
     * a unit rather than a date. A calendar year is a different question and
     * the Date mode answers that one. */
    { "year", 31557600.0 },
};

static const struct calc_unit CALC_DATA[] = {
    { "bit", 0.125 }, { "byte", 1.0 },
    { "kilobyte", 1024.0 }, { "megabyte", 1048576.0 },
    { "gigabyte", 1073741824.0 }, { "terabyte", 1099511627776.0 },
    { "petabyte", 1125899906842624.0 },
};

static const struct calc_unit CALC_PRESSURE[] = {
    { "pascal", 1.0 }, { "kilopascal", 1000.0 }, { "bar", 100000.0 },
    { "atmosphere", 101325.0 }, { "pound per square inch", 6894.757293168 },
    { "millimetre of mercury", 133.322387415 },
};

static const struct calc_unit CALC_ENERGY[] = {
    { "joule", 1.0 }, { "kilojoule", 1000.0 }, { "calorie", 4.184 },
    { "kilocalorie", 4184.0 }, { "watt hour", 3600.0 },
    { "kilowatt hour", 3600000.0 }, { "BTU", 1055.05585262 },
    { "electronvolt", 1.602176634e-19 },
};

static const struct calc_unit CALC_POWER[] = {
    { "watt", 1.0 }, { "kilowatt", 1000.0 }, { "megawatt", 1000000.0 },
    { "horsepower", 745.6998715822702 },
};

static const struct calc_unit CALC_ANGLE[] = {
    { "degree", 1.0 }, { "radian", 57.29577951308232 },
    { "gradian", 0.9 }, { "turn", 360.0 },
    { "arcminute", 0.016666666666666666 },
    { "arcsecond", 0.0002777777777777778 },
};

#define CALC_UNITS(a) (a), ((int)(sizeof(a) / sizeof((a)[0])))

static const struct calc_category CALC_CATEGORIES[] = {
    { "Length",      "metre",  CALC_UNITS(CALC_LENGTH),      false },
    { "Weight",      "kilogram", CALC_UNITS(CALC_MASS),      false },
    { "Temperature", "Celsius", CALC_UNITS(CALC_TEMPERATURE), true  },
    { "Area",        "square metre", CALC_UNITS(CALC_AREA),  false },
    { "Volume",      "litre",  CALC_UNITS(CALC_VOLUME),      false },
    { "Speed",       "metre per second", CALC_UNITS(CALC_SPEED), false },
    { "Time",        "second", CALC_UNITS(CALC_TIME),        false },
    { "Data",        "byte",   CALC_UNITS(CALC_DATA),        false },
    { "Pressure",    "pascal", CALC_UNITS(CALC_PRESSURE),    false },
    { "Energy",      "joule",  CALC_UNITS(CALC_ENERGY),      false },
    { "Power",       "watt",   CALC_UNITS(CALC_POWER),       false },
    { "Angle",       "degree", CALC_UNITS(CALC_ANGLE),       false },
};

#define CALC_CATEGORY_COUNT \
    ((int)(sizeof(CALC_CATEGORIES) / sizeof(CALC_CATEGORIES[0])))

/*
 * Currency is deliberately absent.
 *
 * Every other family here is a ratio fixed by definition: an inch has been
 * exactly 0.0254 metres since 1959 and will be tomorrow. An exchange rate is
 * a fact about this afternoon, and a calculator that shipped with one baked
 * in would give confidently wrong answers to the one question where being
 * wrong costs money.
 *
 * It needs a rate source over the network, which ReconOS can now reach and
 * encrypt but has no service to ask. When there is one, it is another entry
 * in this table with its factors filled in at runtime and a line saying when
 * they were last fetched -- because a rate without a timestamp is the same
 * problem in a nicer font.
 */

#endif /* RECON_CALC_MODES_H */
