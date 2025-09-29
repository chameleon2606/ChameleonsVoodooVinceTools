#include <iostream>
#include <fstream>
#include <sstream>
#include <regex>
#include <stack>
#include <map>
#include <vector>
#include "include/json.hpp"

using json = nlohmann::json;

// Fix decimal numbers without leading zero
std::string fixDecimals(const std::string &input) {
    std::regex re(R"((^|[^0-9])(\.[0-9]+))");
    return std::regex_replace(input, re, "$10$2");
}

// Split a string by spaces
std::vector<std::string> splitValues(const std::string &s) {
    std::vector<std::string> result;
    std::istringstream iss(s);
    std::string token;
    while (iss >> token) {
        result.push_back(token);
    }
    return result;
}

// Try to convert a string into number if possible
json convertValue(const std::string &val) {
    std::string fixed = fixDecimals(val);

    // number?
    try {
        if (fixed.find('.') != std::string::npos) {
            return std::stod(fixed);
        } else {
            return std::stoi(fixed);
        }
    } catch (...) {
        // fallback: string
        return fixed;
    }
}

std::string parse_tdf(std::string& filepath) {

    std::ifstream infile(filepath);
    if (!infile) {
        std::cerr << "Failed to open input file\n";
        return "";
    }

    json root;
    std::stack<json*> context; 
    std::stack<std::string> currentKeys;

    context.push(&root);

    std::string line;
    while (std::getline(infile, line)) {
        // trim whitespace
        line.erase(0, line.find_first_not_of(" \t\r\n"));
        line.erase(line.find_last_not_of(" \t\r\n") + 1);

        if (line.empty() || line[0] == '/' ) continue; // skip empty or comments

        if (line.front() == '[' && line.back() == ']') {
            // section name
            std::string section = line.substr(1, line.size() - 2);
            currentKeys.push(section);
        } 
        else if (line == "{") {
            std::string key = currentKeys.top();
            currentKeys.pop();
            json &parent = *context.top();
            parent[key] = json::object();
            context.push(&parent[key]);
        } 
        else if (line == "}") {
            context.pop();
        } 
        else {
            // key = value;
            auto pos = line.find('=');
            if (pos == std::string::npos) continue;

            std::string key = line.substr(0, pos);
            std::string value = line.substr(pos + 1);

            // clean key/value
            key.erase(0, key.find_first_not_of(" \t"));
            key.erase(key.find_last_not_of(" \t;") + 1);

            value.erase(0, value.find_first_not_of(" \t"));
            value.erase(value.find_last_not_of(" \t;") + 1);

            // multiple values? (space-separated)
            auto tokens = splitValues(value);

            json parsedValue;
            if (tokens.size() > 1) {
                parsedValue = json::array();
                for (auto &t : tokens) {
                    parsedValue.push_back(convertValue(t));
                }
            } else {
                parsedValue = convertValue(value);
            }

            (*context.top())[key] = parsedValue;
        }
    }

    //std::cout << root.dump(4) << std::endl;
    return root.dump(4);
}