#ifndef JL_JITUTIL_H
#define JL_JITUTIL_H
#include<vector>
#include<unordered_set>
#include<llvm/Support/raw_ostream.h>
#include "julia.h"
extern "C" JL_DLLEXPORT const char *jl_name_from_method_instance(jl_method_instance_t *li);
enum OutputFileType { ObjFile, UnoptIR, OptIR };
class ObjectPathGenerator {
public:
    virtual std::string getPath(std::string miName, OutputFileType fty)
    {
        std::string rootP = root;
        miName = "jlmethod::" + miName;
        miName = normalizeName(miName);
        llvm::raw_string_ostream(rootP) << '/' << miName;
        if (fty == ObjFile) {
            llvm::raw_string_ostream(rootP) << ".o";
        }
        else if (fty == UnoptIR) {
            llvm::raw_string_ostream(rootP) << ".ll";
        }
        else if (fty == OptIR) {
            llvm::raw_string_ostream(rootP) << "-opt.ll";
        }
        return rootP;
    }
    void setRootPath(std::string &path) { root = path; }
    std::string encodeNum(size_t num)
    {
        std::string s;
        while (num > 0) {
            uint8_t hex = num & 0b1111;
            llvm::raw_string_ostream(s)
                << (hex > 10 ? (char)('A' + (hex - 10)) : (char)('0' + hex));
            num = num >> 4;
        }
        return s;
    }
    std::string makeSafe(std::string name)
    {
        std::replace_if(
            name.begin(), name.end(), [](const char &c) { return (c == '/'); }, ' ');
        return name;
    }
    std::string normalizeName(std::string name)
    {
        name = makeSafe(name);
        if (name.length() > 220) {
            size_t salt = hasher(name);
            name = name.substr(0, 220);
            llvm::raw_string_ostream(name) << encodeNum(salt);
            return name;
        }
        else {
            return name;
        }
    }

private:
    std::string root;
    std::hash<std::string> hasher;
};

template<typename T>
class UniqueVector {
public:
    void push_back(T e)
    {
        if (elements.find(e) == elements.end()) {
            queue.push_back(e);
            elements.insert(e);
        }
    }
    bool has(T e) { return elements.find(e) != elements.end(); }
    std::vector<T> queue;
    std::unordered_set<T> elements;
};
inline bool startsWith(std::string mainStr, std::string toMatch)
{
    // std::string::find returns 0 if toMatch is found at starting
    if (mainStr.find(toMatch) == 0)
        return true;
    else
        return false;
};

#endif
