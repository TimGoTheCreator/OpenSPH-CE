# OpenSPH Community Edition (OpenSPH-CE)

This is a fork of OpenSPH extending file format support and platform compatibility.

The original project was developed by Pavel Ševeček at the Astronomical Institute 
of Charles University (https://gitlab.com/sevecekp/sph).

## Additions in Community Edition

### Extended I/O formats

- **HDF5**: Native reading and writing of simulation data in HDF5 format. 
  Supports standard OpenSPH fields as well as cosmological datasets from 
  SWIFT, GADGET-2/4, and AREPO (`/PartType0`, `/PartType1`, and `/Header` attributes).
- **Alembic**: Export and import of point particle data to `.abc` format for 
  use in external 3D software and renderers.

### Solvers

- Optimization of neighbor distance calculations in `AsymmetricSolver` and 
  `EnergyConservingSolver`.

### Build system

- Windows build support using MSYS2 and UCRT64 toolchain.

## Compilation

Community Edition adds optional dependencies:
- HDF5 library (enabled by `-DWITH_HDF5=ON` or when HDF5 is detected)
- Alembic library (enabled by `-DWITH_ALEMBIC=ON` or when Alembic is detected)

Compilation with CMake:
```bash
mkdir build
cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make
