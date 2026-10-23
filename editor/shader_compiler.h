#pragma once

#include <shader.h>

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// The editor's kast::ShaderCompiler: runs the shaderc.exe next to this program (the one CMake compiles the built-in
// shaders with, same flags) on generateShader's output for the mesh template copied beside it (kshader/, see
// CMakeLists.txt). Never linked into the player.
bool compileShader(const kast::ShaderSource& source, kast::ShaderStage stage, std::vector<uint8_t>& bytecode,
                   std::string& error);
// The shader cache's version (ShaderLibrary::setCompiler): a hash of what the bytecode depends on besides the .kshader,
// the templates and includes under kshader/ and shaderc.exe (its size and time), so rebuilding either misses the cache.
std::string shaderToolsVersion();
// The folder of this program (UTF-8 from SDL), where CMake puts shaderc.exe, texturec.exe and kshader/.
std::filesystem::path toolDir();
// Runs args[0] with args (SDL quotes each; no console window) and waits for it to exit: what it printed (stdout and
// stderr) in output, its exit code in code. False + error if it can't start.
bool runTool(const std::vector<std::string>& args, std::string& output, int& code, std::string& error);
