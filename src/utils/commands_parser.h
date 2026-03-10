#include "../types/types.h"

class CommandsParser {
public:
    void promptUserForCommand(std::string& command);
    types::AppCommand parseCommand(std::string& command);

private:
    bool isValidCommand(std::string& command);
    std::vector<std::string> tokenizeCommand(std::string& command);
};