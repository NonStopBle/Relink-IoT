// Camera (OpenGL) -- watch a device's live camera in an OpenGL window, peer-to-peer.
//
// Dashboard setup:  V10 Camera, V11 ByteArray
// Pairs with:       the Arduino "CameraStream" example
// Needs:            freeglut + libjpeg (apt install freeglut3-dev libjpeg-dev)
// Run:              ./relink_camera_gl            (Esc or q quits)
//                   ./relink_camera_gl 20         (quit after 20 s)
// You'll see:       the video drawn as a GL texture, "15.0 fps ..." in the title and terminal.
#include <relink/relink.hpp>
#include <GL/freeglut.h>
#include <jpeglib.h>
#include <csetjmp>
#include <chrono>
#include <mutex>
#include <vector>
#include "settings.hpp"   // 1. Your settings live here (camera needs a KEY: P2P mesh)

static const uint8_t STREAM_PIN = 10;

// Decoded on the receive thread; the GL thread only uploads.
static std::mutex mtx;
static std::vector<uint8_t> rgb;     // newest frame, RGB
static int fw = 0, fh = 0;
static bool fresh = false;
static long frames = 0, bytes = 0;

struct JpegErr { jpeg_error_mgr mgr; jmp_buf jump; };

static bool decodeJpeg(const uint8_t* jpeg, size_t len, std::vector<uint8_t>& out, int& w, int& h) {
    jpeg_decompress_struct c;
    JpegErr err;
    c.err = jpeg_std_error(&err.mgr);
    // A broken frame must not kill the program: jump back here instead of exit().
    err.mgr.error_exit = [](j_common_ptr ci) { longjmp(((JpegErr*)ci->err)->jump, 1); };
    if (setjmp(err.jump)) { jpeg_destroy_decompress(&c); return false; }
    jpeg_create_decompress(&c);
    jpeg_mem_src(&c, jpeg, (unsigned long)len);
    jpeg_read_header(&c, TRUE);
    c.out_color_space = JCS_RGB;
    c.dct_method = JDCT_IFAST;
    jpeg_start_decompress(&c);
    w = c.output_width; h = c.output_height;
    out.resize((size_t)w * h * 3);
    while (c.output_scanline < c.output_height) {
        uint8_t* row = &out[(size_t)c.output_scanline * w * 3];
        jpeg_read_scanlines(&c, &row, 1);
    }
    jpeg_finish_decompress(&c);
    jpeg_destroy_decompress(&c);
    return true;
}

static relink::Client* client;
static GLuint tex;
static int texW = 0, texH = 0;
static double secs = 1e18;
static auto t0 = std::chrono::steady_clock::now(), tLast = t0;

static double since(std::chrono::steady_clock::time_point t) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count();
}

static void display() {
    {
        std::lock_guard<std::mutex> g(mtx);
        if (fresh) {
            glBindTexture(GL_TEXTURE_2D, tex);
            if (fw != texW || fh != texH) {   // allocate once per resolution, then update in place
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fw, fh, 0, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
                texW = fw; texH = fh;
            } else {
                glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, fw, fh, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());
            }
            fresh = false;
        }
    }
    int ww = glutGet(GLUT_WINDOW_WIDTH), wh = glutGet(GLUT_WINDOW_HEIGHT);
    glViewport(0, 0, ww, wh);
    glClearColor(0.05f, 0.05f, 0.06f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    if (texW) {
        // Letterbox: keep the camera's aspect ratio inside the window
        float sx = std::min(1.0f, (float)wh * texW / texH / ww);
        float sy = std::min(1.0f, (float)ww * texH / texW / wh);
        glBegin(GL_QUADS);
        glTexCoord2f(0, 1); glVertex2f(-sx, -sy);
        glTexCoord2f(1, 1); glVertex2f( sx, -sy);
        glTexCoord2f(1, 0); glVertex2f( sx,  sy);
        glTexCoord2f(0, 0); glVertex2f(-sx,  sy);
        glEnd();
    }
    glutSwapBuffers();

    if (since(tLast) >= 2) {
        long n, b;
        { std::lock_guard<std::mutex> g(mtx); n = frames; b = bytes; frames = bytes = 0; }
        char msg[128];
        if (texW) snprintf(msg, sizeof msg, "%5.1f fps  %4.1f KB  %dx%d", n / since(tLast), n ? b / 1024.0 / n : 0.0, texW, texH);
        else snprintf(msg, sizeof msg, "waiting for frames");
        char title[160]; snprintf(title, sizeof title, "Relink camera (OpenGL) -- %s", msg);
        glutSetWindowTitle(title);
        printf("[gl-cpp] %s | ", msg);
        printStatus(*client);
        tLast = std::chrono::steady_clock::now();
    }
    if (since(t0) >= secs) glutLeaveMainLoop();
}

int main(int argc, char** argv) {
    if (argc > 1) secs = atof(argv[1]);

    // 2. Connect
    relink::Client r(RELINK_TOKEN_, RELINK_KEY_, RELINK_SERVER_);
    client = &r;

    // 3. Each frame arrives as JPEG bytes: decode here, off the GL thread
    std::vector<uint8_t> buf;
    r.onCamera(STREAM_PIN, [&](const uint8_t* jpeg, size_t len, uint16_t) {
        int w, h;
        if (!decodeJpeg(jpeg, len, buf, w, h)) return;
        std::lock_guard<std::mutex> g(mtx);
        rgb.swap(buf); fw = w; fh = h; fresh = true;
        frames++; bytes += (long)len;
    });
    r.start();

    // 4. OpenGL window: one texture, redrawn continuously
    glutInit(&argc, argv);
    glutInitDisplayMode(GLUT_DOUBLE | GLUT_RGB);
    glutInitWindowSize(640, 480);
    glutCreateWindow("Relink camera (OpenGL)");
    glutSetOption(GLUT_ACTION_ON_WINDOW_CLOSE, GLUT_ACTION_GLUTMAINLOOP_RETURNS);
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glEnable(GL_TEXTURE_2D);
    glutDisplayFunc(display);
    glutIdleFunc(glutPostRedisplay);
    glutKeyboardFunc([](unsigned char k, int, int) { if (k == 27 || k == 'q') glutLeaveMainLoop(); });
    glutMainLoop();
    r.stop();
}
