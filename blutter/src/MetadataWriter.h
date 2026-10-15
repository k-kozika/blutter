#pragma once

#include <string>
#include <vector>

#include "DartApp.h"

class MetadataWriter {
public:
    explicit MetadataWriter(DartApp& app) : app(app) {}
    // Empty selectors preserve the full metadata output. Selectors match a
    // class name or an exact library URL followed by :: and the class name.
    void Create(const char* filename, const std::vector<std::string>& selectors = {});

private:
    DartApp& app;
};
