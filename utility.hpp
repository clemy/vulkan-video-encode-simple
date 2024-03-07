/*
 * Vulkan Video Encode Extension - Simple Example

 * Copyright (c) 2024 Bernhard C. Schrenk <clemy@clemy.org>
 *   University of Vienna
 *   https://www.univie.ac.at/
 * developed during the courses "Cloud Gaming" & "Practical Course 1"
 *   supervised by Univ.-Prof. Dipl.-Ing. Dr. Helmut Hlavacs <helmut.hlavacs@univie.ac.at>
 *     University of Vienna
 *     Research Group "EDEN - Education, Didactics and Entertainment Computing"
 *     https://eden.cs.univie.ac.at/
 *
 * This file is licensed under the MIT license.
 * See the LICENSE file in the project root for full license information.
 */

#include <fstream>
#include <stdexcept>
#include <string>

#define VK_CHECK(x)                                                                                           \
    do {                                                                                                      \
        VkResult result = (x);                                                                                \
        if (result != VK_SUCCESS) {                                                                           \
            throw std::runtime_error("Error: " #x " returned " + std::to_string(result) + " in " + __FILE__ + \
                                     " at line " + std::to_string(__LINE__));                                 \
        }                                                                                                     \
    } while (0)

static std::vector<char> readFile(const std::string &filename) {
    std::ifstream inputFile(filename, std::ios::binary);
    if (!inputFile) {
        throw std::runtime_error("failed to open file: " + filename);
    }
    return {(std::istreambuf_iterator<char>(inputFile)), {}};
}
