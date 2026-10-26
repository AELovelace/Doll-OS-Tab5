//   Calc.ino
//   handles the "calc" command via the tinyexpr library. Ported from DOLL-OS's
//   calc.ino -- pure math, no display dependency, only the output calls changed.
#include "tinyexpr.h"

void handleCalcCommand(const String parts[], int partCount) {
    if (partCount == 1) {
        showCalcHelp(0);
    } else if (parts[1] == "help") {
        showCalcHelp(1);
    } else if (partCount == 2) {
        int success = 1;
        const char* expression = parts[1].c_str();
        double expressionResult = te_interp(expression, &success);
        outLine(String(expression), C_PINK);
        outLine(String(expressionResult), C_CYAN);
    } else {
        showCalcHelp(2);
    }
}

void showCalcHelp(int helpMsg) {
    switch (helpMsg) {
        case 0:
            outLine("Syntax: calc (expression)", C_PINK);
            outLine("calc help for details", C_CYAN);
            break;
        case 1:
            outLine("Supported Functions", C_PINK);
            outLine("BASIC: + - * / ^ %");
            outLine("abs acos asin atan atan2 ceil");
            outLine("cos cosh exp floor ln log");
            outLine("log10 pow sin sinh sqrt tan");
            outLine("tanh");
            outLine("SPACES NOT SUPPORTED, USE '_'", C_CYAN);
            outLine("POWERED BY codeplea/tinyexpr", C_YELLOW);
            break;
        case 2:
            outLine("only two arguments, no spaces", C_RED);
            outLine("Syntax: calc (expression)", C_GREEN);
            break;
    }
}
