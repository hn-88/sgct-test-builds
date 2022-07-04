/*****************************************************************************************
 * SGCT                                                                                  *
 * Simple Graphics Cluster Toolkit                                                       *
 *                                                                                       *
 * Copyright (c) 2012-2022                                                               *
 * For conditions of distribution and use, see copyright notice in LICENSE.md            *
 ****************************************************************************************/

#ifndef __SGCT__ENGINE__H__
#define __SGCT__ENGINE__H__

#include <sgct/actions.h>
#include <sgct/callbackdata.h>
#include <sgct/config.h>
#include <sgct/frustum.h>
#include <sgct/joystick.h>
#include <sgct/keys.h>
#include <sgct/modifiers.h>
#include <sgct/mouse.h>
#include <sgct/window.h>
#include <array>
#include <functional>
#include <optional>
#include <thread>

namespace sgct {

struct Configuration;
class Node;
class StatisticsRenderer;

// The `path` should be an absolute path or relative to the current working directory
config::Cluster loadCluster(std::optional<std::string> path);

/**
 * The Engine class is the central part of sgct and handles most of the callbacks,
 * rendering, network handling, input devices etc.
 */
class Engine {
public:
    // Structure with all statistics gathered over the frame. The newest value is always
    // at the front of the array
    struct Statistics {
        static inline const int HistoryLength = 128;

        std::array<double, HistoryLength> frametimes = {};
        std::array<double, HistoryLength> drawTimes = {};
        std::array<double, HistoryLength> syncTimes = {};
        std::array<double, HistoryLength> loopTimeMin = {};
        std::array<double, HistoryLength> loopTimeMax = {};

        /// \return the frame time (delta time) in seconds
        double dt() const;

        /// \return the average frame time (delta time) in seconds
        double avgDt(unsigned int frameCounter) const;

        /// \return the minimum frame time (delta time) in the averaging window (seconds)
        double minDt() const;

        /// \return the maximum frame time (delta time) in the averaging window (seconds)
        double maxDt() const;

    };

    struct Callbacks {
        /// This function is called before the window is created (before OpenGL context is
        /// created). At this stage the configuration file has been read and network
        /// is initialized.
        std::function<void()> preWindow;

        /// This function is called once before the starting the render loop and after
        /// creation of the OpenGL context.
        std::function<void(GLFWwindow*)> initOpenGL;

        /// This function is called before the synchronization stage
        std::function<void()> preSync;

        /// This function is called once per frame after sync but before draw stage
        std::function<void()> postSyncPreDraw;

        /// This function draws the scene and could be called several times per frame
        /// as it's called once per viewport and once per eye if stereoscopy is used.
        std::function<void(const RenderData&)> draw;

        /// This function is be called after overlays and post effects has been drawn and
        /// can used to render text and HUDs that will not be filtered or antialiased.
        std::function<void(const RenderData&)> draw2D;

        /// This function is called after the draw stage but before the OpenGL buffer swap
        std::function<void()> postDraw;

        /// This is called before all SGCT components will be destroyed
        std::function<void()> cleanup;

        /// This function is called to encode all shared data that is sent to the
        /// connected nodes in a clustered setup.
        std::function<std::vector<std::byte>()> encode;

        /// This function is called by decode all shared data sent to us from the master
        /// The first parameter is the block of data that contains the data to be decoded,
        /// the second parameter is the position in the data at which to start the
        /// decoding.
        std::function<void(const std::vector<std::byte>&, unsigned int)> decode;

        /// This function is called when a TCP message is received
        std::function<void(const char*, int)> externalDecode;

        /// This function is called when the connection status changes
        std::function<void(bool)> externalStatus;

        /// This function is called when a TCP message is received
        std::function<void(void*, int, int, int)> dataTransferDecode;

        /// This function is called when the connection status changes
        std::function<void(bool, int)> dataTransferStatus;

        /// This function is called when data is successfully sent
        std::function<void(int, int)> dataTransferAcknowledge;

        /// This function sets the keyboard callback (GLFW wrapper) for all windows
        std::function<void(Key, Modifier, Action, int)> keyboard;

        /// All windows are connected to this callback.
        std::function<void(unsigned int, int)> character;

        /// This function sets the mouse button callback (GLFW wrapper) for all windows
        std::function<void(MouseButton, Modifier, Action)> mouseButton;

        /// All windows are connected to this callback.
        std::function<void(double, double)> mousePos;

        /// All windows are connected to this callback.
        std::function<void(double, double)> mouseScroll;

        /// Drop files to any window. All windows are connected to this callback.
        std::function<void(int, const char**)> drop;
    };

    static Engine& instance();

    /**
     * \param cluster The cluster setup that should be used for this SGCT run
     * \param callbacks The list of callbacks that should be installed
     */
    static void create(config::Cluster cluster, Callbacks callbacks,
        const Configuration& arg);
    static void destroy();

    /// Terminates SGCT
    void terminate();

    /// This is SGCT's renderloop where rendering & synchronization takes place
    void render();

    /// Returns the statistic object containing all information about the frametimes, etc
    const Statistics& statistics() const;

    /// \return the clear color as 4 floats (RGBA)
    vec4 clearColor() const;

    /// \return the near clipping plane distance in meters
    float nearClipPlane() const;

    /// \return the far clipping plane distance in meters
    float farClipPlane() const;

    /**
     * Set the near and far clipping planes. This operation recalculates all frustums for
     * all viewports.
     *
     * \param nearClippingPlane near clipping plane in meters
     * \param farClippingPlane far clipping plane in meters
     */
    void setNearAndFarClippingPlanes(float nearClippingPlane, float farClippingPlane);

    /**
     * Set the eye separation (interocular distance) for all users. This operation
     * recalculates all frustums for all viewports.
     *
     * \param eyeSeparation eye separation in meters
     */
    void setEyeSeparation(float eyeSeparation);

    /**
     * Set the clear color (background color).
     *
     * \param color the clear color
     */
    void setClearColor(vec4 color);

    /**
     * This functions updates the frustum of all viewports on demand. However if the
     * viewport is tracked this is done on the fly.
     */
    void updateFrustums();

    /// \return the index of the focus window. If no window has focus, nullptr is returned
    const Window* focusedWindow() const;

    /// Sets if the statistics graph should be rendered or not
    void setStatsGraphVisibility(bool state);

    /**
     * Take an RGBA screenshot and save it as a PNG file. If stereo rendering is enabled
     * then two screenshots will be saved per frame, one for each eyeo.
     *
     * To record frames for a movie simply call this function every frame you wish to
     * record. The read to disk is multi-threaded.
     *
     * \param windowIds If the vector is empty, screenshots of all windows will be taken,
     *        otherwise, only the window ids that appear in the vector will be used for
     *        screenshots and window ids that do not appear in the list are ignored
     */
    void takeScreenshot(std::vector<int> windowIds = std::vector<int>());

    /// Set the screenshot number (file index)
    void setScreenShotNumber(unsigned int number);

    /// \return the current screenshot number (file index)
    unsigned int screenShotNumber() const;

    /**
     * This function returns the currently assigned draw function to be used in internal
     * classes that need to repeatedly call this. In general, there is no need for
     * external applications to store the draw function, but they are free to do so. Be
     * aware that the user (i.e. you) is allowed to change the draw function at any time.
     *
     * \return The currently bound draw function
     */
    const std::function<void(const RenderData&)>& drawFunction() const;

    /// Get the time from program start in seconds
    static double getTime();

    /// \return a reference to this node (running on this computer).
    const Node& thisNode() const;

    /// \return A list of all the windows for the current node
    const std::vector<std::unique_ptr<Window>>& windows() const;

    /// \return a pointer to the user (observer position) object
    static User& defaultUser();

    /// \return true if this node is the master
    bool isMaster() const;

    /// Returns the current frame number
    unsigned int currentFrameNumber() const;

    /**
     * Specifies the sync parameters to be used in the rendering loop.
     *
     * \param printMessage If <code>true</code> a message is print waiting for a frame
     *        every second
     * \param timeout The timeout that the master and clients will wait for in seconds
     */
    void setSyncParameters(bool printMessage = true, float timeout = 60.f);

    /**
     * Set up the current viewport, the framebuffer resolutions, windowing, and scissoring
     * in OpenGL. This is a function that is called by internal classes of SGCT and in
     * general does not have to be called by any external application using this library.
     */
    void setupViewport(const Window& window, const BaseViewport& viewport,
        Frustum::Mode frustum);

private:
    static Engine* _instance;

    Engine(config::Cluster cluster, Callbacks callbacks, const Configuration& arg);

    /// Engine destructor destructs GLFW and releases resources/memory.
    ~Engine();

    void initialize();

    /// Create and initiate a window.
    void initWindows(int majorVersion, int minorVersion);

    /**
     * Locks the rendering thread for synchronization. Locks the clients until data is
     * successfully received.
     */
    void frameLockPreStage();

    /**
     * Locks the rendering thread for synchronization. Locks master until clients are
     * ready to swap buffers.
     */
    void frameLockPostStage();

    /// Draw viewport overlays if there are any.
    void drawOverlays(const Window& window, Frustum::Mode frustum);

    /**
     * Draw geometry and bind FBO as texture in screenspace (ortho mode). The geometry can
     * be a simple quad or a geometry correction and blending mesh.
     */
    void renderFBOTexture(Window& window);

    /// This function combines a texture and a shader into a new texture
    void renderFXAA(Window& window, Window::TextureIndex targetIndex);

    void renderViewports(Window& window, Frustum::Mode frustum, Window::TextureIndex ti);

    /// This function renders stats, OSD and overlays
    void render2D(const Window& window, Frustum::Mode frustum);

    /**
     * This function waits for all windows to be created on the whole cluster in order to
     * set the barrier (hardware swap-lock). Under some Nvidia drivers the stability is
     * improved by first join a swapgroup and then set the barrier then all windows in a
     * swapgroup are created.
     */
    void waitForAllWindowsInSwapGroupToOpen();

    /**
     * This function copies/render the result from the previous window same viewport (if
     * it exists) into this window
     */
    void blitWindowViewport(Window& prevWindow, Window& window,
        const Viewport& viewport, Frustum::Mode mode);

    const std::function<void()> _preWindowFn;
    const std::function<void(GLFWwindow*)> _initOpenGLFn;
    const std::function<void()> _preSyncFn;
    const std::function<void()> _postSyncPreDrawFn;
    const std::function<void(const RenderData&)> _drawFn;
    const std::function<void(const RenderData&)> _draw2DFn;
    const std::function<void()> _postDrawFn;
    const std::function<void()> _cleanupFn;

    float _nearClipPlane = 0.1f;
    float _farClipPlane = 100.f;
    vec4 _clearColor = vec4{ 0.f, 0.f, 0.f, 1.f };

    Statistics _statistics;
    double _statsPrevTimestamp = 0.0;
    std::unique_ptr<StatisticsRenderer> _statisticsRenderer;

    bool _createDebugContext = false;
    bool _takeScreenshot = false;
    std::vector<int> _takeScreenshotIds;
    bool _shouldTerminate = false;

    bool _printSyncMessage = true;
    float _syncTimeout = 60.f;

    struct FXAAShader {
        ShaderProgram shader;
        int sizeX = -1;
        int sizeY = -1;
        int subPixTrim = -1;
        int subPixOffset = -1;
    };
    std::optional<FXAAShader> _fxaa;
    ShaderProgram _fboQuad;
    ShaderProgram _overlay;

    std::unique_ptr<std::thread> _thread;

    unsigned int _frameCounter = 0;
    unsigned int _shotCounter = 0;
};

} // namespace sgct

#endif // __SGCT__ENGINE__H__
