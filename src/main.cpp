// PRE-COMPILED HEADERS
#include "pch.hpp"

// ENGINE
#include "core/engine.hpp"

int main() {
    devwarnln("Current channel is DEV, extended debugging is enabled");
    println("Initializing engine");

    Engine engine;
    if (!engine.init()) {
        warnln("Engine initialisation failed — shutting down");
        engine.shutdown();
        return exitp(1);
    }

    engine.run();
    engine.shutdown();

    println("Clean shutdown");
    return exitp(0);
}
