#ifndef SWAMP_PARSER_HPP
#define SWAMP_PARSER_HPP

#include <iostream>
#include <fstream>
#include <string>
#include <vector>
#include <sstream>
#include <cmath>
#include <stdexcept>
#include <json.hpp>

using json = nlohmann::json;

class SwampParser {
public:
    /**
     * Parses a custom configuration file (.tdf) into a JSON object.
     * 
     * @param filepath Path to the .tdf file.
     * @return nlohmann::json Parsed JSON object.
     * @throws std::runtime_error if the file cannot be opened.
     */
    static json parseFile(const std::string& filepath) {
        std::ifstream file(filepath);
        if (!file.is_open()) {
            throw std::runtime_error("SwampParser Error: Could not open file at " + filepath);
        }
        
        return parseStream(file);
    }

    /**
     * Parses a custom configuration string/stream into a JSON object.
     * 
     * @param is Input stream containing the data.
     * @return nlohmann::json Parsed JSON object.
     */
    static json parseStream(std::istream& is) {
        std::vector<std::pair<std::string, json>> stack;
        stack.push_back({"", json::object()}); // Root object

        std::string line;
        std::string pendingKey = "";

        while (std::getline(is, line)) {
            line = trim(line);
            
            // Skip empty lines and comments
            if (line.empty() || line.substr(0, 2) == "//") {
                continue;
            }

            // Match Section Headers, e.g., [Objects]
            if (line.front() == '[' && line.back() == ']') {
                pendingKey = line.substr(1, line.size() - 2);
            } 
            // Match Block Start
            else if (line == "{") {
                stack.push_back({pendingKey, json::object()});
                pendingKey = "";
            } 
            // Match Block End
            else if (line == "}") {
                if (stack.size() > 1) {
                    auto top = stack.back();
                    stack.pop_back();

                    auto& parent = stack.back().second;
                    
                    // Handle duplicate keys by grouping them into a JSON array
                    if (parent.contains(top.first)) {
                        if (parent[top.first].is_array()) {
                            parent[top.first].push_back(top.second);
                        } else {
                            json arr = json::array();
                            arr.push_back(parent[top.first]);
                            arr.push_back(top.second);
                            parent[top.first] = arr;
                        }
                    } else {
                        parent[top.first] = top.second;
                    }
                }
            } 
            // Match Key-Value pairs, e.g., Location = 1.0 2.0 3.0;
            else {
                auto eqPos = line.find('=');
                if (eqPos != std::string::npos) {
                    std::string key = trim(line.substr(0, eqPos));
                    std::string value = trim(line.substr(eqPos + 1));
                    
                    // Remove trailing semicolon
                    if (!value.empty() && value.back() == ';') {
                        value.pop_back();
                        value = trim(value);
                    }

                    // For top level items like "Version = 20;" which may lack a section
                    if (!pendingKey.empty() && stack.size() == 1) {
                        pendingKey = ""; 
                    }

                    stack.back().second[key] = parseValue(value);
                }
            }
        }
        
        return stack.front().second;
    }

private:
    // Helper to remove whitespace from ends of strings
    static std::string trim(const std::string& s) {
        auto start = s.find_first_not_of(" \t\r\n");
        auto end = s.find_last_not_of(" \t\r\n");
        return (start == std::string::npos) ? "" : s.substr(start, end - start + 1);
    }

    // Intelligently infer data types from the parsed string value
    static json parseValue(const std::string& val) {
        // Boolean evaluation
        if (val == "true") return true;
        if (val == "false") return false;

        // Try parsing numerical arrays or single numbers
        std::istringstream iss(val);
        std::vector<double> nums;
        std::string token;
        bool is_numeric = true;
        
        while (iss >> token) {
            try {
                size_t pos;
                double num = std::stod(token, &pos);
                // If there are trailing non-numeric characters, it's a string
                if (pos != token.length()) {
                    is_numeric = false;
                    break;
                }
                nums.push_back(num);
            } catch (...) {
                is_numeric = false;
                break;
            }
        }

        if (is_numeric && !nums.empty()) {
            if (nums.size() == 1) {
                // If it evaluates as an integer and has no decimal point, store as int
                if (nums[0] == std::floor(nums[0]) && val.find('.') == std::string::npos) {
                    return static_cast<long long>(nums[0]);
                }
                return nums[0]; // Store as float
            } else {
                return nums; // Store space-separated numbers as a JSON Array
            }
        }
        
        // Fallback: Return raw string. This ensures values with spaces 
        // (like material texture names) remain flawlessly intact as strings.
        return val;
    }
};

#endif // SWAMP_PARSER_HPP