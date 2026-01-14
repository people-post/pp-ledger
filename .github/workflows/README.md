# GitHub Actions Workflows

This directory contains automated workflows for building and testing the pp-ledger project.

## Workflows

### build-project.yml

**Main build and test workflow**

- **Triggers:** Push to main, pull requests, manual dispatch
- **Purpose:** Build and test the pp-ledger project
- **Features:**
  - Installs system dependencies (OpenSSL, nlohmann-json)
  - Builds all components including network library
  - Runs all tests

**Steps:**
1. Checkout code
2. Install system dependencies
3. Configure CMake
4. Build with `make`
5. Run tests with `ctest`

## Usage

### Running the Workflow

The main build runs automatically on pushes and PRs. To manually trigger:

1. Go to **Actions** tab
2. Select **Build pp-ledger**
3. Click **Run workflow**
4. Select branch and click **Run workflow**

## Dependencies

The workflow installs the following system packages:
- build-essential (GCC, make, etc.)
- cmake
- libssl-dev
- nlohmann-json3-dev

## Troubleshooting

### Build Failures

If builds fail:
1. Check workflow logs for specific errors
2. Ensure all dependencies are correctly specified
3. Verify the CMakeLists.txt configuration is correct

## Future Improvements

Planned enhancements:
- [ ] Add caching for faster builds
- [ ] Create release artifacts
- [ ] Add deployment workflows
- [ ] Run tests in parallel
- [ ] Add code coverage reporting
