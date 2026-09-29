// Window events and remapped controls for the native input layer.

#include <fstream>
#include <sstream>

#include "host.hpp"
#include "petari/input.hpp"
#include "petari/input_sdl3.hpp"

namespace PetariNative::App::Events {

bool loadControls(const std::filesystem::path& file, std::string* error) {
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        return true;  // defaults
    }
    std::ifstream in(file);
    if (!in) {
        *error = "cannot read " + file.string();
        return false;
    }
    std::stringstream text;
    text << in.rdbuf();
    Input::Bindings bindings = Input::Bindings::defaults();
    std::string parseError;
    if (!bindings.parse(text.str(), &parseError)) {
        *error = file.string() + ": " + parseError;
        return false;
    }
    Input::setBindings(bindings);
    return true;
}

bool input(const SDL_Event& event) {
    return Input::SDL3::handleEvent(event);
}

void setImage(const Rect& image, float windowWidth, float windowHeight) {
    Input::Viewport viewport;
    viewport.windowWidth = windowWidth;
    viewport.windowHeight = windowHeight;
    viewport.imageX = image.x;
    viewport.imageY = image.y;
    viewport.imageWidth = image.width;
    viewport.imageHeight = image.height;
    Input::setViewport(viewport);
}

}  // namespace PetariNative::App::Events
