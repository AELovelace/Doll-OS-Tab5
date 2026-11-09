//   Dice.ino
//   dice roller. Ported verbatim from DOLL-OS's dice.ino aside from the output calls.
void diceHelp() {
    outLine("Syntax: Dice [numSides] [numRolls]", C_CYAN);
}

void handleDiceCommand(const String parts[], int partCount) {
    int diceSides = 6;
    int diceNumber = 1;
    if (partCount == 1) {
        diceHelp();
        return;
    } else {
        if (parts[1].length() != 0) {
            diceSides = parts[1].toInt();
        }
        if (parts[2].length() != 0) {
            diceNumber = parts[2].toInt();
        }
    }
    diceRoll(diceSides, diceNumber);
}

void diceRoll(int sides, int rolls) {
    String result[rolls];
    int color = random(3);
    String resultPrint = "Your roll: ";
    for (int i = 0; i < rolls; i++) {
        result[i] = random(sides) + 1;
        if (i < rolls - 1) {
            resultPrint += result[i] + ", ";
        } else {
            resultPrint += result[i];
        }
    }
    switch (color) {
        case 0: outLine(resultPrint, C_CYAN); break;
        case 1: outLine(resultPrint, C_PINK); break;
        case 2: outLine(resultPrint, C_GREEN); break;
    }
}
