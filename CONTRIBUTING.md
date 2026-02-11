# Contributing to Distributed Cache System

Thank you for your interest in contributing! This document provides guidelines for contributing to the project.

## 🚀 Getting Started

1. **Fork** the repository
2. **Clone** your fork locally
3. **Create** a feature branch (`git checkout -b feature/amazing-feature`)
4. **Make** your changes
5. **Test** your changes
6. **Commit** with a clear message
7. **Push** to your fork
8. **Open** a Pull Request

## 📋 Development Setup

### Prerequisites

- C++17 compatible compiler (GCC 6.3+, MSVC 2017+, Clang 5+)
- Git

### Building

```bash
# Windows (MinGW)
g++ -std=c++17 -O2 -I. -o build/distributed_cache.exe src/main.cpp -lws2_32

# Linux/Mac
g++ -std=c++17 -O2 -I. -o build/distributed_cache src/main.cpp -pthread
```

### Running Tests

```bash
# All tests
.\demo\run_all_tests.ps1   # Windows
./demo/run_all_tests.sh    # Linux/Mac

# Individual test suites
./build/test_lru_cache
./build/test_concurrency
./build/test_resp_parser
```

## 📝 Code Style

- **Naming**: `snake_case` for functions/variables, `PascalCase` for classes
- **Indentation**: 4 spaces (no tabs)
- **Braces**: K&R style
- **Comments**: Use `//` for inline, `/** */` for documentation

### Example

```cpp
/**
 * Brief description of the function.
 * @param key The cache key
 * @param value The value to store
 * @return true on success
 */
bool put(const std::string& key, const std::string& value) {
    if (key.empty()) {
        return false;  // Early return for validation
    }
    
    // Main logic here
    cache_[key] = value;
    return true;
}
```

## 🧪 Testing Guidelines

- Add tests for new features
- Maintain existing test coverage
- Use the existing test framework pattern:

```cpp
TEST(test_my_new_feature) {
    // Arrange
    MyClass obj;
    
    // Act
    auto result = obj.do_something();
    
    // Assert
    assert(result == expected);
}
```

## 🔀 Pull Request Process

1. **Update** the README.md if needed
2. **Add** tests for new functionality
3. **Ensure** all tests pass
4. **Update** documentation
5. **Request** review from maintainers

### PR Checklist

- [ ] Code compiles without warnings
- [ ] All tests pass
- [ ] Documentation updated
- [ ] Commit messages are clear
- [ ] No unnecessary files included

## 📜 Commit Messages

Use clear, descriptive commit messages:

```
feat: add support for EXPIRE command
fix: resolve race condition in segment locking
docs: update README with new examples
test: add unit tests for LRU eviction
refactor: simplify cache manager interface
```

## 🐛 Reporting Bugs

Please include:

1. **Description** of the bug
2. **Steps to reproduce**
3. **Expected behavior**
4. **Actual behavior**
5. **Environment** (OS, compiler version)

## 💡 Feature Requests

We welcome feature requests! Please:

1. **Check** existing issues first
2. **Describe** the use case
3. **Propose** a solution if possible

## 📧 Questions?

Open an issue with the "question" label or reach out to the maintainers.

---

Thank you for contributing! 🎉
