#pragma once

#include "io/Output.h"

NAMESPACE_SPH_BEGIN

/// \brief Saves particle data as an Alembic (.abc) point cache.
class AlembicOutput : public IOutput {
private:
    Float scale = 1._f;

public:
    explicit AlembicOutput(const OutputFile& fileMask, const Float scale = 1._f);
    ~AlembicOutput();

    virtual Expected<Path> dump(const Storage& storage, const Statistics& stats) override;
};

/// \brief Loads particle data from an Alembic (.abc) point cache into Storage.
class AlembicInput : public IInput {
public:
    virtual Outcome load(const Path& path, Storage& storage, Statistics& stats) override;
};

NAMESPACE_SPH_END
