# GitHub Actions Setup

This document describes how to set up and use GitHub Actions for the pp-ledger project.

## Overview

The project uses GitHub Actions for continuous integration and Docker image releases. CI runners and the container image both use **Ubuntu 24.04**.

## Workflow Configuration

### Build Workflow (`build-project.yml`)

The main workflow handles:
1. Installing system dependencies
2. Configuring CMake
3. Building the project
4. Running tests

### Release Workflow (`release-docker.yml`)

On `release/v*` tags (and manual dispatch), the workflow:
1. Builds the multi-stage `Dockerfile` (`ubuntu:24.04`)
2. Pushes `ghcr.io/<owner>/pp-ledger:<version>` (and `:latest` for tags)
3. Attaches a linux-x64 binary tarball to a GitHub Release

See `.github/workflows/README.md` and `deploy/README.md` for details.

### Required Dependencies

The workflow installs:
- **build-essential**: GCC, make, and other build tools
- **cmake**: Build system generator

Value/Meta JSON IO lives in **pp-cpp-common** (`common/io/Json.h`). Crypto is provided by pp-cpp-crypto (FetchContent).

## Running Locally

To build the project locally:

```bash
# Install dependencies (Ubuntu/Debian)
sudo apt-get update
sudo apt-get install -y \
    build-essential \
    cmake

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
