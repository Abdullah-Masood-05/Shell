#pragma once

// A file's icon as pixels the UI can draw: a program ("C:\x\app.exe", or
// "C:\x\app.dll,3" for one of its icons), a shortcut (its own icon, as Explorer
// shows it), a document, or a bare program name Windows finds ("calc.exe").
// Read-only; call it off the UI thread with COM initialized.

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct IconPixels
{
    uint32_t width = 0;
    uint32_t height = 0;
    std::vector<uint8_t> rgba; // straight RGBA, top-down
};

std::optional<IconPixels> programIcon(const std::string &source, int size = 32);
