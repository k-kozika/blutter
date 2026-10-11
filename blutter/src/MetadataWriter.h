#pragma once

#include "DartApp.h"

class MetadataWriter {
public:
    explicit MetadataWriter(DartApp& app) : app(app) {}
    void Create(const char* filename);

private:
    DartApp& app;
};
