// stdlib.cpp - Axon source of the standard library modules, embedded in the compiler.
#include <cstring>

#include "runtime.h"

static const char* STD_IO = R"AX(
// std.io - reading and writing text.
extern "@axon" {
    string? readLine();
    Result<string, string> readFile(string path);
    bool writeFile(string path, string text);
    bool appendFile(string path, string text);
    bool fileExists(string path);
    bool deleteFile(string path);
    string readAll(string path);      // the whole file, or "" if it can't be read
    string[] args();
}
)AX";

static const char* STD_OS = R"AX(
// std.os - the operating system: run commands, environment variables, exit.
extern "@axon" {
    int run(string command);          // runs a shell command, returns its exit code
    string? env(string name);         // an environment variable, or none
    void exit(int code);              // ends the program right away
    int argCount();                   // number of command-line arguments
    string arg(int i);                // argument i (from 0), or ""
}
)AX";

static const char* STD_TEXT = R"AX(
// std.text - characters and joining text. Written in Axon.
bool isDigit(char c) {
    return c >= '0' && c <= '9';
}

bool isLetter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c >= (char)0xC0;
}

bool isLetterOrDigit(char c) {
    return isLetter(c) || isDigit(c);
}

bool isSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

bool isUpper(char c) {
    return c >= 'A' && c <= 'Z';
}

bool isLower(char c) {
    return c >= 'a' && c <= 'z';
}

char toUpperChar(char c) {
    if (isLower(c)) {
        return (char)(c - 32);
    }
    return c;
}

char toLowerChar(char c) {
    if (isUpper(c)) {
        return (char)(c + 32);
    }
    return c;
}

string fromChar(char c) {
    return "" + c;
}

string join(ref string[] parts, string separator) {
    string out = "";
    for (int i = 0; i < parts.length; i++) {
        if (i > 0) {
            out += separator;
        }
        out += parts[i];
    }
    return out;
}

string joinList(ref List<string> parts, string separator) {
    string out = "";
    for (int i = 0; i < parts.length; i++) {
        if (i > 0) {
            out += separator;
        }
        out += parts[i];
    }
    return out;
}
)AX";

static const char* STD_MATH = R"AX(
// std.math - numbers.
extern "math.h" {
    double sqrt(double x);
    double pow(double x, double y);
    double sin(double x);
    double cos(double x);
    double tan(double x);
    double atan2(double y, double x);
    double floor(double x);
    double ceil(double x);
    double round(double x);
    double fabs(double x);
    double exp(double x);
    double log(double x);
    double fmod(double x, double y);
    double hypot(double x, double y);
    double asin(double x);
    double acos(double x);
    double atan(double x);
}

const double PI = 3.141592653589793;
const double E = 2.718281828459045;

int abs(int x) {
    if (x < 0) {
        return -x;
    }
    return x;
}

int min(int a, int b) {
    if (a < b) {
        return a;
    }
    return b;
}

int max(int a, int b) {
    if (a > b) {
        return a;
    }
    return b;
}

long absl(long x) {
    if (x < 0) {
        return -x;
    }
    return x;
}

double lerp(double a, double b, double t) {
    return a + (b - a) * t;
}

double toRadians(double degrees) {
    return degrees * PI / 180.0;
}

double toDegrees(double radians) {
    return radians * 180.0 / PI;
}

double distance(double x1, double y1, double x2, double y2) {
    return sqrt((x2 - x1) * (x2 - x1) + (y2 - y1) * (y2 - y1));
}

int clamp(int value, int lo, int hi) {
    if (value < lo) {
        return lo;
    }
    if (value > hi) {
        return hi;
    }
    return value;
}

double minf(double a, double b) {
    if (a < b) {
        return a;
    }
    return b;
}

double maxf(double a, double b) {
    if (a > b) {
        return a;
    }
    return b;
}
)AX";

static const char* STD_RANDOM = R"AX(
// std.random - pseudo-random numbers (not for cryptography).
extern "@axon" {
    void randomSeed(long seed);
    int randomInt(int lo, int hi);      // lo..hi, both included
    double randomDouble();              // 0.0 <= x < 1.0
}
)AX";

static const char* STD_TIME = R"AX(
// std.time - clocks and waiting.
extern "@axon" {
    long timeMillis();
    void sleepMillis(int ms);
}
)AX";

const char* stdModuleSource(const char* name) {
    if (!strcmp(name, "io")) return STD_IO;
    if (!strcmp(name, "math")) return STD_MATH;
    if (!strcmp(name, "random")) return STD_RANDOM;
    if (!strcmp(name, "time")) return STD_TIME;
    if (!strcmp(name, "os")) return STD_OS;
    if (!strcmp(name, "text")) return STD_TEXT;
    return nullptr;
}
