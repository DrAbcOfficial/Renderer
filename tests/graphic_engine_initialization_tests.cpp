#include "graphic_engine_initialization.h"
#include <cassert>
#include <cstdio>

int main()
{
    CGraphicEngineInitialization state;

    int firstContext  = 0;
    int secondContext = 0;

    assert(GraphicEngineInitialization_Initialize == state.Begin(&firstContext));
    assert(GraphicEngineInitialization_Reentrant == state.Begin(&firstContext));
    assert(GraphicEngineInitialization_Reentrant == state.Begin(&secondContext));

    //Failure leaves initialization retryable.
    state.Finish();
    assert(GraphicEngineInitialization_Initialize == state.Begin(&firstContext));

    state.Complete(&firstContext);
    state.Finish();
    assert(GraphicEngineInitialization_Ready == state.Begin(&firstContext));
    assert(GraphicEngineInitialization_Ready == state.Begin(&firstContext));

    assert(GraphicEngineInitialization_Initialize == state.Begin(&secondContext));
    state.Complete(&secondContext);
    state.Finish();
    assert(GraphicEngineInitialization_Ready == state.Begin(&secondContext));

    //Shutdown invalidates even a subsequently reused context handle.
    state.Reset();
    assert(GraphicEngineInitialization_Initialize == state.Begin(&secondContext));
    state.Finish();

    assert(GraphicEngineInitialization_Initialize == state.Begin(nullptr));
    state.Complete(nullptr);
    state.Finish();
    assert(GraphicEngineInitialization_Initialize == state.Begin(nullptr));

    //The legacy path creates its context during initialization.
    state.Complete(&firstContext);
    state.Finish();
    assert(GraphicEngineInitialization_Ready == state.Begin(&firstContext));

    std::puts("graphic_engine_initialization_tests passed");
}
