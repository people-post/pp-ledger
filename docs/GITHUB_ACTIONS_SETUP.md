# GitHub Actions Setup

This document describes how to set up and use GitHub Actions for the pp-ledger project.

## Overview

The project uses GitHub Actions for continuous integration (CI), automatically building and testing the code on every push and pull request.

## Workflow Configuration

### Build Workflow (`build-project.yml`)

The main workflow handles:
1. Installing system dependencies
2. Configuring CMake
3. Building the project
4. Running tests

### Required Dependencies

The workflow installs:
- **build-essential**: GCC, make, and other build tools
- **cmake**: Build system generator
- **libssl-dev**: OpenSSL development libraries
- **libboost-all-dev**: Boost libraries
- **libfmt-dev**: fmt formatting library
- **nlohmann-json3-dev**: JSON library for C++
- **python3**: Python interpreter (for build scripts)

## Running Locally

To build the project locally:

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake \
    libssl-dev \
    libboost-all-dev \
    libfmt-dev \
    nlohmann-json3-dev

# Configure and build
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)

# Run tests
ctest --output-on-failure
```

## Triggering Workflows

### Automatic Triggers
- Push to `main` branch
- Pull requests to `main` branch

### Manual Trigger
1. Go to the Actions tab in GitHub
2. Select "Build pp-ledger"
3. Click "Run workflow"
4. Select branch and confirm

## Troubleshooting

### Common Issues

1. **Missing dependencies**: Ensure all system packages are installed
2. **CMake errors**: Check CMakeLists.txt for syntax errors
3. **Build failures**: Review compiler error messages in logs
4. **Test failures**: Check test output for specific failure reasons

### Getting Help

- Check the workflow logs in GitHub Actions
- Review recent commits for potentially breaking changes
- Open an issue with detailed error information
