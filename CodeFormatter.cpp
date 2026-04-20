#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef enum {
    STATE_NORMAL,
    STATE_STRING,
    STATE_CHAR,
    STATE_LINE_COMMENT,
    STATE_BLOCK_COMMENT
} ParserState;

char* removeComments(const char* code) {
    int len = strlen(code);
    char* result = (char*)malloc(len * 2 + 1);
    int j = 0;
    ParserState state = STATE_NORMAL;

    for (int i = 0; i < len; i++) {
        char c = code[i];
        char next = (i + 1 < len) ? code[i + 1] : 0;

        switch (state) {
            case STATE_NORMAL:
                if (c == '"') {
                    result[j++] = c;
                    state = STATE_STRING;
                } else if (c == '\'') {
                    result[j++] = c;
                    state = STATE_CHAR;
                } else if (c == '/' && next == '/') {
                    state = STATE_LINE_COMMENT;
                    i++;
                } else if (c == '/' && next == '*') {
                    state = STATE_BLOCK_COMMENT;
                    i++;
                } else {
                    result[j++] = c;
                }
                break;

            case STATE_STRING:
                result[j++] = c;
                if (c == '\\' && next != 0) {
                    result[j++] = code[++i];
                } else if (c == '"') {
                    state = STATE_NORMAL;
                }
                break;

            case STATE_CHAR:
                result[j++] = c;
                if (c == '\\' && next != 0) {
                    result[j++] = code[++i];
                } else if (c == '\'') {
                    state = STATE_NORMAL;
                }
                break;

            case STATE_LINE_COMMENT:
                if (c == '\n') {
                    result[j++] = c;
                    state = STATE_NORMAL;
                }
                break;

            case STATE_BLOCK_COMMENT:
                if (c == '*' && next == '/') {
                    state = STATE_NORMAL;
                    i++;
                }
                break;
        }
    }

    result[j] = '\0';
    return result;
}

int isIdentifierChar(char c) {
    return isalnum(c) || c == '_';
}

int isTypeKeyword(const char* str, int len) {
    static const char* keywords[] = {"int", "char", "float", "double", "void", "long", "short", "unsigned", "signed", "const", "static", "class", "struct", "enum", "union", "typedef", "explicit", "virtual", "override", "final", "vector", "list", "map", "set", "unordered_map", "unordered_set", "array"};
    for (int i = 0; i < 27; i++) {
        int kwlen = strlen(keywords[i]);
        if (len == kwlen && strncmp(str, keywords[i], kwlen) == 0) {
            return 1;
        }
    }
    return 0;
}

char* optimizeSpaces(const char* code) {
    int len = strlen(code);
    char* result = (char*)malloc(len * 2 + 1);
    int j = 0;
    int i = 0;

    while (i < len) {
        if (code[i] == ' ' || code[i] == '\t') {
            i++;
            if (i >= len) break;

            char curr = code[i];
            char next = (i + 1 < len) ? code[i + 1] : 0;
            char prev = (j > 0) ? result[j - 1] : 0;

            int shouldKeepSpace = 0;

            if (isIdentifierChar(prev) && isIdentifierChar(curr)) {
                shouldKeepSpace = 1;
            }

            if (shouldKeepSpace) {
                result[j++] = ' ';
            }

            if ((curr == '+' && next == '+') || (curr == '-' && next == '-')) {
                result[j++] = curr;
                result[j++] = next;
                i += 2;
            } else {
                result[j++] = curr;
                i++;
            }
        } else {
            result[j++] = code[i++];
        }
    }

    result[j] = '\0';

    return result;
}

char* addSpecialSpaces(const char* code) {
    int len = strlen(code);
    char* result = (char*)malloc(len * 3 + 1);
    int j = 0;
    int angleDepth = 0;
    int i = 0;

    while (i < len) {
        result[j++] = code[i];

        if (code[i] == '<') {
            int k = i - 1;
            while (k >= 0 && (code[k] == ' ' || code[k] == '\t')) k--;
            if (k >= 0) {
                if (isIdentifierChar(code[k]) || code[k] == '*' || code[k] == '&') {
                    int start = k;
                    while (start >= 0 && isIdentifierChar(code[start])) start--;
                    start++;
                    int keywordLen = k - start + 1;
                    if (isTypeKeyword(code + start, keywordLen)) {
                        angleDepth++;
                    }
                }
            }
            i++;
            continue;
        }

        if (code[i] == '>') {
            if (angleDepth > 0) {
                angleDepth--;
                if (i + 1 < len && !isspace(code[i + 1]) && code[i + 1] != '\0' && code[i + 1] != ';' && code[i + 1] != ')' && code[i + 1] != ']' && code[i + 1] != ',' && code[i + 1] != '+' && code[i + 1] != '-' && code[i + 1] != '*' && code[i + 1] != '/' && code[i + 1] != '%' && code[i + 1] != '^' && code[i + 1] != '&' && code[i + 1] != '|' && code[i + 1] != '=') {
                    result[j++] = ' ';
                }
            }
            i++;
            continue;
        }

        if (code[i] == '&' || code[i] == '*') {
            if (i + 1 < len && !isspace(code[i + 1]) && code[i + 1] != '\0') {
                if (isIdentifierChar(code[i + 1])) {
                    int k = i - 1;
                    while (k >= 0 && (code[k] == ' ' || code[k] == '\t')) k--;

                    int isDeclaration = 0;
                    if (k >= 0) {
                        if (code[k] == '*' || code[k] == '&') {
                            isDeclaration = 1;
                        } else if (isIdentifierChar(code[k])) {
                            int start = k;
                            while (start >= 0 && isIdentifierChar(code[start])) start--;
                            start++;
                            if (isTypeKeyword(code + start, k - start + 1)) {
                                isDeclaration = 1;
                            } else {
                                char next = code[i + 1];
                                if (isupper(next)) {
                                    isDeclaration = 1;
                                }
                            }
                        } else if (code[k] == ')') {
                            isDeclaration = 1;
                        } else if (code[k] == ':') {
                            isDeclaration = 1;
                        }
                    }

                    if (isDeclaration) {
                        result[j++] = ' ';
                    }
                }
            }
        }

        i++;
    }

    result[j] = '\0';
    return result;
}

char* removeAllBlankLines(const char* code) {
    int len = strlen(code);
    char* result = (char*)malloc(len * 2 + 1);
    int j = 0;

    int lineStart = 0;

    for (int i = 0; i <= len; i++) {
        if (i == len || code[i] == '\n') {
            int lineEnd = i;

            int isBlank = 1;
            for (int k = lineStart; k < lineEnd; k++) {
                if (code[k] != ' ' && code[k] != '\t') {
                    isBlank = 0;
                    break;
                }
            }

            if (!isBlank) {
                for (int k = lineStart; k < lineEnd; k++) {
                    result[j++] = code[k];
                }
                if (i < len && code[i] == '\n') {
                    result[j++] = '\n';
                }
            }

            lineStart = i + 1;
        }
    }

    if (j > 0 && result[j - 1] == '\n') {
        j--;
    }

    result[j] = '\0';
    return result;
}

char* formatCode(const char* code) {
    char* step1 = removeComments(code);
    char* step2 = removeAllBlankLines(step1);
    char* step3 = optimizeSpaces(step2);
    char* step4 = addSpecialSpaces(step3);

    free(step1);
    free(step2);
    free(step3);

    return step4;
}

int main() {
    char buffer[100000];
    int pos = 0;
    int c;

    while ((c = getchar()) != EOF && pos < 99999) {
        buffer[pos++] = c;
    }
    buffer[pos] = '\0';

    char* formatted = formatCode(buffer);
    printf("%s", formatted);
    free(formatted);

    system("pause");
    
    return 0;
}
