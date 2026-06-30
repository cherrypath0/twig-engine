#include "pch.hpp"
#include "material/material.hpp"
#include "gpu/gpu.hpp"

#include <cctype>
#include <fstream>
#include <sstream>
#include <vector>

#include "stb_image.h"   // STB_IMAGE_IMPLEMENTATION lives in thirdparty.cpp

namespace material {

Material make_default() { return Material{}; }

// ----------------------------------------------------------------------------- tokenizer
namespace {

struct Token { std::string text; bool is_string = false; };

std::vector<Token> tokenize(const std::string& src) {
    std::vector<Token> out;
    size_t i = 0, n = src.size();
    while (i < n) {
        char c = src[i];
        // whitespace
        if (std::isspace(static_cast<unsigned char>(c))) { ++i; continue; }
        // line comment
        if (c == '/' && i + 1 < n && src[i + 1] == '/') {
            while (i < n && src[i] != '\n') ++i;
            continue;
        }
        // block comment
        if (c == '/' && i + 1 < n && src[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(src[i] == '*' && src[i + 1] == '/')) ++i;
            i += 2;
            continue;
        }
        // quoted string
        if (c == '"') {
            ++i;
            std::string s;
            while (i < n && src[i] != '"') s.push_back(src[i++]);
            ++i; // closing quote
            out.push_back({s, true});
            continue;
        }
        // single-char punctuation
        if (c == '{' || c == '}' || c == '[' || c == ']') {
            out.push_back({std::string(1, c), false});
            ++i;
            continue;
        }
        // bare word / number
        std::string w;
        while (i < n) {
            char d = src[i];
            if (std::isspace(static_cast<unsigned char>(d)) ||
                d == '{' || d == '}' || d == '[' || d == ']' || d == '"') break;
            w.push_back(d);
            ++i;
        }
        if (!w.empty()) out.push_back({w, false});
    }
    return out;
}

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

void apply_key(Material& m, const std::string& key,
               const std::vector<Token>& vals) {
    if (vals.empty()) return;
    std::string k = lower(key);
    auto num = [&](int i) -> float {
        if (i >= (int)vals.size()) return 0.0f;
        try { return std::stof(vals[i].text); } catch (...) { return 0.0f; }
    };

    if (k == "shader") {
        m.shader = vals[0].text;
    } else if (k == "g_tcolor" || k == "color" || k == "basecolor" || k == "albedo") {
        m.color_texture = vals[0].text;
    } else if (k == "g_vcolortint" || k == "tint" || k == "color_tint") {
        m.color_tint = {num(0), num(1), num(2), vals.size() > 3 ? num(3) : 1.0f};
    } else if (k == "g_flmetalness" || k == "metalness" || k == "metallic") {
        m.metalness = num(0);
    } else if (k == "g_flroughness" || k == "roughness") {
        m.roughness = num(0);
    } else if (k == "g_flambient" || k == "ambient") {
        m.ambient = num(0);
    } else {
        devwarnln("tgmat: ignoring unknown key '%s'", key.c_str());
    }
}

} // namespace

bool load_tgmat(const std::string& path, Material& out) {
    std::ifstream f(path);
    if (!f) { warnln("tgmat: cannot open %s", path.c_str()); return false; }
    std::stringstream ss; ss << f.rdbuf();
    std::vector<Token> toks = tokenize(ss.str());

    out = make_default();
    out.name = std::filesystem::path(path).stem().string();

    // Find the opening brace of the top-level block (e.g. after "Material").
    size_t i = 0;
    while (i < toks.size() && toks[i].text != "{") ++i;
    if (i >= toks.size()) { warnln("tgmat: no block found in %s", path.c_str()); return false; }
    ++i; // past '{'

    while (i < toks.size() && toks[i].text != "}") {
        const std::string key = toks[i].text;
        ++i;
        std::vector<Token> vals;
        if (i < toks.size() && toks[i].text == "[") {
            ++i;
            while (i < toks.size() && toks[i].text != "]") vals.push_back(toks[i++]);
            if (i < toks.size()) ++i; // past ']'
        } else if (i < toks.size() && toks[i].text != "}") {
            vals.push_back(toks[i++]);
        }
        apply_key(out, key, vals);
    }

    println("Loaded material '%s' (shader=%s)", out.name.c_str(), out.shader.c_str());
    return true;
}

void resolve_gpu(SDL_GPUDevice* dev, Material& mat, SDL_GPUTexture* white_fallback) {
    mat.use_texture = false;
    mat.tex = white_fallback;
    if (mat.color_texture.empty()) return;

    int w = 0, h = 0, ch = 0;
    stbi_set_flip_vertically_on_load(0);
    unsigned char* pixels = stbi_load(mat.color_texture.c_str(), &w, &h, &ch, 4);
    if (!pixels) {
        warnln("tgmat: texture '%s' not found, using flat tint", mat.color_texture.c_str());
        return;
    }
    mat.tex = gpu::create_texture_rgba(dev, pixels, (uint32_t)w, (uint32_t)h, mat.color_texture.c_str());
    stbi_image_free(pixels);
    if (mat.tex) {
        mat.use_texture = true;
    } else {
        mat.tex = white_fallback;
    }
}

} // namespace material
