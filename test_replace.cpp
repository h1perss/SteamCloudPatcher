#include <iostream>
#include <string>

int main() {
    std::string content = "\"cloudenabled\"\t\t\"1\"";
    size_t pos = 0;
    while ((pos = content.find("\"cloudenabled\"", pos)) != std::string::npos) {
        size_t valPos = content.find("\"1\"", pos);
        if (valPos != std::string::npos && valPos < pos + 50) {
            content.replace(valPos, 3, "\"0\"");
        }
        pos += 14;
    }
    std::cout << content << std::endl;
    return 0;
}
