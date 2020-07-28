/*
 * This file Copyright (C) 2012-2015 Mnemosyne LLC
 *
 * It may be used under the GNU GPL versions 2 or 3
 * or any future license endorsed by Mnemosyne LLC.
 *
 */

#pragma once

#include <array>
#include <cstdint> // int64_t

#include <QCoreApplication> // Q_DECLARE_TR_FUNCTIONS
#include <QString>

class Speed;

class Formatter
{
    Q_DECLARE_TR_FUNCTIONS(Formatter)

public:
    enum Size
    {
        B,
        KB,
        MB,
        GB,
        TB,

        NUM_SIZES
    };

    enum Type
    {
        SPEED,
        SIZE,
        MEM,

        NUM_TYPES
    };

public:
    static QString memToString(int64_t bytes);
    static QString sizeToString(int64_t bytes);
    static QString speedToString(Speed const& speed);
    static QString percentToString(double x);
    static QString ratioToString(double ratio);
    static QString timeToString(int seconds);
    static QString uploadSpeedToString(Speed const& up);
    static QString downloadSpeedToString(Speed const& down);

    static QString unitStr(Type t, Size s)
    {
        return UnitStrings[t][s];
    }

    static void initUnits();

private:
    static std::array<std::array<QString, Formatter::NUM_SIZES>, Formatter::NUM_TYPES> const UnitStrings;
};
