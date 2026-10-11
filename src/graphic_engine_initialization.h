#pragma once

enum GraphicEngineInitializationResult
{
    GraphicEngineInitialization_Initialize = 0,
    GraphicEngineInitialization_Ready,
    GraphicEngineInitialization_Reentrant
};

/*
    Render-thread lifecycle guard for InitializeGraphicEngine;
    this does not make GL initialization thread-safe.
*/
class CGraphicEngineInitialization
{
public:
    /*
        Ready:      the given context has already been initialized, nothing to do.
        Reentrant:  called while an initialization is in progress, reject.
        Initialize: the caller must initialize, then call Complete (on success) and Finish.
    */
    GraphicEngineInitializationResult Begin(void* context)
    {
        if (m_Initializing)
            return GraphicEngineInitialization_Reentrant;

        if (context && context == m_Context)
            return GraphicEngineInitialization_Ready;

        m_Context      = nullptr;
        m_Initializing = true;

        return GraphicEngineInitialization_Initialize;
    }

    //Record the initialized context; only called on success, so a failure allows a retry.
    void Complete(void* context)
    {
        m_Context = context;
    }

    //End the initialization started by Begin, whether it succeeded or not.
    void Finish()
    {
        m_Initializing = false;
    }

    //Forget the initialized context on shutdown.
    void Reset()
    {
        m_Context = nullptr;
    }

private:
    void* m_Context{};
    bool  m_Initializing{};
};
