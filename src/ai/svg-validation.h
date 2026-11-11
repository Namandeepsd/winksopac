// SPDX-License-Identifier: GPL-2.0-or-later
#ifndef INKSCAPE_AI_SVG_VALIDATION_H
#define INKSCAPE_AI_SVG_VALIDATION_H
#include <string>
namespace Inkscape::AI {
// Shared restricted generated-artwork validator; no document or GTK access.
std::string validate_generated_svg(std::string svg);
}
#endif
