#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <errno.h>
#include <exception>
#include <fcntl.h>
#include <glib-unix.h>
#include <glib.h>
#include <gst/app/gstappsink.h>
#include <gst/gst.h>
#include <iomanip>
#include <iostream>
#include <memory>
#include <mutex>
#include <signal.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "include/network_helper.hpp"

// Network Protocol Structures
struct CameraRequestData {
  int width;
  int height;
  int fps;
  int bitrate;
  int enableMvHevc;
  int renderMode;
  int port;
  std::string camera;
  std::string ip;

  CameraRequestData()
      : width(1280), height(720), fps(30), bitrate(4000), enableMvHevc(0),
        renderMode(0), port(0) {}
};

struct NetworkDataProtocol {
  std::string command;
  int length;
  std::vector<uint8_t> data;

  NetworkDataProtocol() : length(0) {}
  NetworkDataProtocol(const std::string &cmd, const std::vector<uint8_t> &d)
      : command(cmd), data(d), length(d.size()) {}
};

// Deserialization functions
class CameraRequestDeserializer {
public:
  static CameraRequestData deserialize(const std::vector<uint8_t> &data) {
    if (data.size() < 10) {
      throw std::invalid_argument("Data is too small for valid camera request");
    }

    size_t offset = 0;

    if (data[offset] != 0xCA || data[offset + 1] != 0xFE) {
      throw std::invalid_argument("Invalid magic bytes");
    }
    offset += 2;

    uint8_t version = data[offset++];
    if (version != 1) {
      throw std::invalid_argument("Unsupported protocol version");
    }

    CameraRequestData result;

    if (offset + 28 > data.size()) {
      throw std::invalid_argument("Data too small for integer fields");
    }

    result.width = readInt32(data, offset);
    result.height = readInt32(data, offset + 4);
    result.fps = readInt32(data, offset + 8);
    result.bitrate = readInt32(data, offset + 12);
    result.enableMvHevc = readInt32(data, offset + 16);
    result.renderMode = readInt32(data, offset + 20);
    result.port = readInt32(data, offset + 24);
    offset += 28;

    result.camera = readCompactString(data, offset);
    result.ip = readCompactString(data, offset);

    return result;
  }

private:
  static int32_t readInt32(const std::vector<uint8_t> &data, size_t offset) {
    if (offset + 4 > data.size()) {
      throw std::out_of_range("Not enough data to read int32");
    }
    return static_cast<int32_t>((data[offset]) | (data[offset + 1] << 8) |
                                (data[offset + 2] << 16) |
                                (data[offset + 3] << 24));
  }

  static std::string readCompactString(const std::vector<uint8_t> &data,
                                       size_t &offset) {
    if (offset >= data.size()) {
      throw std::out_of_range("Not enough data to read string length");
    }

    uint8_t length = data[offset++];
    if (length == 0) {
      return std::string();
    }

    if (offset + length > data.size()) {
      throw std::out_of_range("Not enough data to read string content");
    }

    std::string result(reinterpret_cast<const char *>(&data[offset]), length);
    offset += length;
    return result;
  }
};

class NetworkDataProtocolDeserializer {
public:
  static NetworkDataProtocol deserialize(const std::vector<uint8_t> &buffer) {
    if (buffer.size() < 8) {
      throw std::invalid_argument("Buffer too small for valid protocol data");
    }

    size_t offset = 0;

    int32_t commandLength = readInt32(buffer, offset);
    offset += 4;

    if (commandLength < 0 ||
        offset + static_cast<size_t>(commandLength) > buffer.size()) {
      throw std::invalid_argument("Invalid command length");
    }

    std::string command;
    if (commandLength > 0) {
      command = std::string(reinterpret_cast<const char *>(&buffer[offset]),
                            commandLength);
      size_t nullPos = command.find('\0');
      if (nullPos != std::string::npos) {
        command = command.substr(0, nullPos);
      }
    }
    offset += commandLength;

    if (offset + 4 > buffer.size()) {
      throw std::invalid_argument("Buffer too small for data length");
    }

    int32_t dataLength = readInt32(buffer, offset);
    offset += 4;

    if (dataLength < 0 ||
        offset + static_cast<size_t>(dataLength) > buffer.size()) {
      throw std::invalid_argument("Invalid data length");
    }

    std::vector<uint8_t> data;
    if (dataLength > 0) {
      data.assign(buffer.begin() + offset,
                  buffer.begin() + offset + dataLength);
    }

    return NetworkDataProtocol(command, data);
  }

private:
  static int32_t readInt32(const std::vector<uint8_t> &data, size_t offset) {
    if (offset + 4 > data.size()) {
      throw std::out_of_range("Not enough data to read int32");
    }
    return static_cast<int32_t>((data[offset]) | (data[offset + 1] << 8) |
                                (data[offset + 2] << 16) |
                                (data[offset + 3] << 24));
  }
};

// Template helper for C++11 make_unique replacement
template <typename T, typename... Args>
std::unique_ptr<T> make_unique_helper(Args &&...args) {
  return std::unique_ptr<T>(new T(std::forward<Args>(args)...));
}

// Global camera configuration
CameraRequestData current_camera_config;

// Thread-safe global state
std::atomic<bool> stop_requested{false};
std::atomic<bool> streaming_active{false};
std::atomic<bool> send_enabled{false};
std::atomic<bool> preview_enabled{false};

// Thread management
std::unique_ptr<std::thread> listen_thread;
std::unique_ptr<std::thread> streaming_thread;
std::mutex config_mutex;
std::condition_variable streaming_cv;
std::mutex streaming_mutex;

// Network components
std::unique_ptr<TCPClient> sender_ptr;
std::unique_ptr<TCPServer> server_ptr;
std::string send_to_server = "";
int send_to_port = 0;

// GMainLoop for preview-only mode
GMainLoop *loop = nullptr;

// Forward declarations
void handleOpenCamera(const std::vector<uint8_t> &data);
void handleCloseCamera(const std::vector<uint8_t> &data);
void startStreamingThread();
void stopStreamingThread();
void streamingThreadFunction();
void listenThreadFunction(const std::string &listen_address);

bool initialize_sender() {
  int retry = 10;
  while (retry > 0 && !sender_ptr && !stop_requested.load()) {
    try {
      sender_ptr = std::unique_ptr<TCPClient>(
          new TCPClient(send_to_server, send_to_port));
      std::cout << "Attempting to connect to " << send_to_server << ":"
                << send_to_port << std::endl;
      sender_ptr->connect();
      return true;
    } catch (const TCPException &e) {
      std::cerr << "Failed to connect to server: " << e.what() << std::endl;
      sender_ptr = nullptr;
    }
    std::this_thread::sleep_for(std::chrono::seconds(1));
    retry--;
  }
  return false;
}

GstFlowReturn on_new_sample(GstAppSink *sink, gpointer user_data) {
  GstSample *sample = gst_app_sink_pull_sample(sink);
  if (!sample)
    return GST_FLOW_ERROR;

  GstBuffer *buffer = gst_sample_get_buffer(sample);
  GstMapInfo map;
  if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
    const uint8_t *data = map.data;
    gsize size = map.size;

    if (send_enabled.load() && sender_ptr && sender_ptr->isConnected() &&
        data && size > 0) {
      try {
        std::vector<uint8_t> packet(4 + size);
        packet[0] = (size >> 24) & 0xFF;
        packet[1] = (size >> 16) & 0xFF;
        packet[2] = (size >> 8) & 0xFF;
        packet[3] = (size)&0xFF;
        std::copy(data, data + size, packet.begin() + 4);

        sender_ptr->sendData(packet);
      } catch (const TCPException &e) {
        std::cerr << "TCP error in on_new_sample: " << e.what() << std::endl;
        streaming_active.store(false);
      } catch (const std::exception &e) {
        std::cerr << "Unexpected error in on_new_sample: " << e.what()
                  << std::endl;
        streaming_active.store(false);
      }
    }

    gst_buffer_unmap(buffer, &map);
  }

  gst_sample_unref(sample);
  return GST_FLOW_OK;
}

void onDataCallback(const std::string &command) {
  std::vector<uint8_t> binaryData(command.begin(), command.end());

  if (binaryData.size() < 4) {
    std::cerr << "Data too small to contain length header" << std::endl;
    return;
  }

  uint32_t bodyLength = (static_cast<uint32_t>(binaryData[0]) << 24) |
                        (static_cast<uint32_t>(binaryData[1]) << 16) |
                        (static_cast<uint32_t>(binaryData[2]) << 8) |
                        static_cast<uint32_t>(binaryData[3]);

  if (4 + bodyLength > binaryData.size()) {
    std::cerr << "Data too small for declared body length. Expected: "
              << (4 + bodyLength) << ", got: " << binaryData.size()
              << std::endl;
    return;
  }

  std::vector<uint8_t> protocolData(binaryData.begin() + 4,
                                    binaryData.begin() + 4 + bodyLength);

  try {
    NetworkDataProtocol protocol =
        NetworkDataProtocolDeserializer::deserialize(protocolData);

    std::cout << "Received protocol command: '" << protocol.command << "'"
              << std::endl;

    if (protocol.command == "OPEN_CAMERA") {
      handleOpenCamera(protocol.data);
    } else if (protocol.command == "CLOSE_CAMERA") {
      handleCloseCamera(protocol.data);
    } else {
      std::cout << "Unknown protocol command: " << protocol.command
                << std::endl;
    }
  } catch (const std::exception &e) {
    std::cout << "Failed to parse as NetworkDataProtocol: " << e.what()
              << std::endl;
  }
}

void onDisconnectCallback() {
  std::cout << "Client disconnected, stopping streaming" << std::endl;
  stopStreamingThread();
}

void listenThreadFunction(const std::string &listen_address) {
  std::cout << "Listen thread started on " << listen_address << std::endl;

  while (!stop_requested.load()) {
    try {
      server_ptr = make_unique_helper<TCPServer>(listen_address);
      server_ptr->setDataCallback(onDataCallback);
      server_ptr->setDisconnectCallback(onDisconnectCallback);
      server_ptr->start();
      std::cout << "TCPServer is listening on " << listen_address << std::endl;

      while (!stop_requested.load() && server_ptr) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
      }

      if (server_ptr) {
        server_ptr->stop();
        server_ptr = nullptr;
      }

      if (!stop_requested.load()) {
        std::cout << "Waiting for new connection..." << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
      }

    } catch (const std::exception &e) {
      std::cerr << "Listen thread error: " << e.what() << std::endl;
      if (!stop_requested.load()) {
        std::this_thread::sleep_for(std::chrono::seconds(2));
      }
    }
  }

  std::cout << "Listen thread stopped" << std::endl;
}

void handleOpenCamera(const std::vector<uint8_t> &data) {
  std::cout << "Handling OPEN_CAMERA command" << std::endl;

  try {
    CameraRequestData cameraConfig =
        CameraRequestDeserializer::deserialize(data);

    std::cout << "Camera config - Width: " << cameraConfig.width
              << ", Height: " << cameraConfig.height
              << ", FPS: " << cameraConfig.fps
              << ", Bitrate: " << cameraConfig.bitrate
              << ", IP: " << cameraConfig.ip << ", Port: " << cameraConfig.port
              << ", type: " << cameraConfig.camera << std::endl;
    // Do nothing if the type is not "WEB"
    if (cameraConfig.camera != "WEB") {
      std::cout << "Unsupported camera type: " << cameraConfig.camera
                << ". Only 'WEB' is supported." << std::endl;
      return;
    }

    {
      std::lock_guard<std::mutex> lock(config_mutex);
      current_camera_config = cameraConfig;
    }

    send_to_server = cameraConfig.ip;
    send_to_port = cameraConfig.port;

    std::cout << "Updated sender target to " << send_to_server << ":"
              << send_to_port << std::endl;

    startStreamingThread();

  } catch (const std::exception &e) {
    std::cerr << "Failed to parse camera config: " << e.what() << std::endl;
    if (!send_to_server.empty() && send_to_port > 0) {
      startStreamingThread();
    } else {
      std::cerr
          << "No valid server configuration available, cannot start streaming"
          << std::endl;
    }
  }
}

void handleCloseCamera(const std::vector<uint8_t> &data) {
  std::cout << "Handling CLOSE_CAMERA command" << std::endl;
  stopStreamingThread();
}

void startStreamingThread() {
  std::lock_guard<std::mutex> lock(streaming_mutex);
  if (streaming_thread && streaming_thread->joinable()) {
    std::cout << "Streaming thread already running" << std::endl;
    return;
  }

  streaming_active.store(true);
  streaming_thread = make_unique_helper<std::thread>(streamingThreadFunction);
  std::cout << "Started streaming thread" << std::endl;
}

void stopStreamingThread() {
  std::lock_guard<std::mutex> lock(streaming_mutex);

  streaming_active.store(false);
  send_enabled.store(false);

  if (sender_ptr && sender_ptr->isConnected()) {
    sender_ptr->disconnect();
  }
  sender_ptr = nullptr;

  if (streaming_thread && streaming_thread->joinable()) {
    streaming_cv.notify_all();
    streaming_thread->join();
    streaming_thread = nullptr;
    std::cout << "Stopped streaming thread" << std::endl;
  }
}

void streamingThreadFunction() {
  std::cout << "Streaming thread started" << std::endl;

  try {
    if (!initialize_sender()) {
      std::cerr << "Failed to initialize sender, streaming thread stopping"
                << std::endl;
      return;
    }

    send_enabled.store(true);

    CameraRequestData config;
    {
      std::lock_guard<std::mutex> lock(config_mutex);
      config = current_camera_config;
    }

    // Build pipeline string for webcam with x264enc
    std::string pipeline_desc;
    std::string src = "v4l2src device=/dev/video0 ! "
                      "video/x-raw,width=" +
                      std::to_string(config.width) +
                      ",height=" + std::to_string(config.height) + " ! "
                      "videoconvert ! video/x-raw,format=I420 ! ";

    if (preview_enabled.load()) {
      pipeline_desc =
          src + "tee name=t "
                "t. ! queue ! x264enc tune=zerolatency bitrate=" +
          std::to_string(config.bitrate) +
          " speed-preset=ultrafast ! "
          "appsink name=mysink emit-signals=true sync=false "
          "t. ! queue ! videoconvert ! ximagesink sync=false";
    } else {
      pipeline_desc = src + "x264enc tune=zerolatency bitrate=" +
                      std::to_string(config.bitrate) +
                      " speed-preset=ultrafast ! "
                      "appsink name=mysink emit-signals=true sync=false";
    }

    g_print("Pipeline: %s\n", pipeline_desc.c_str());

    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(pipeline_desc.c_str(), &error);
    if (!pipeline) {
      g_printerr("Failed to create pipeline: %s\n", error->message);
      g_clear_error(&error);
      return;
    }
    if (error) {
      g_printerr("Pipeline warning: %s\n", error->message);
      g_clear_error(&error);
    }

    GstElement *appsink = gst_bin_get_by_name(GST_BIN(pipeline), "mysink");
    if (!appsink) {
      g_printerr("Failed to get appsink element\n");
      gst_object_unref(pipeline);
      return;
    }

    g_signal_connect(appsink, "new-sample", G_CALLBACK(on_new_sample), nullptr);

    GstStateChangeReturn ret =
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
      g_printerr("Failed to set pipeline to PLAYING state\n");
      gst_object_unref(appsink);
      gst_object_unref(pipeline);
      return;
    }

    std::cout << "Streaming started..." << std::endl;

    // Run GLib main loop on this thread for GStreamer callbacks
    GMainLoop *stream_loop = g_main_loop_new(nullptr, FALSE);

    // Poll for stop signal while running the loop
    GSource *check_source = g_timeout_source_new(100);
    g_source_set_callback(
        check_source,
        [](gpointer user_data) -> gboolean {
          GMainLoop *l = static_cast<GMainLoop *>(user_data);
          if (!streaming_active.load() || stop_requested.load()) {
            g_main_loop_quit(l);
            return G_SOURCE_REMOVE;
          }
          return G_SOURCE_CONTINUE;
        },
        stream_loop, nullptr);
    g_source_attach(check_source, g_main_loop_get_context(stream_loop));
    g_source_unref(check_source);

    g_main_loop_run(stream_loop);

    std::cout << "Streaming loop ended, cleaning up..." << std::endl;

    gst_element_send_event(pipeline, gst_event_new_eos());
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(appsink);
    gst_object_unref(pipeline);
    g_main_loop_unref(stream_loop);

  } catch (const std::exception &e) {
    std::cerr << "Streaming thread error: " << e.what() << std::endl;
  }

  std::cout << "Streaming thread finished" << std::endl;
}

void handle_sigint_threaded(int) {
  std::cout << "\nSIGINT received. Stopping all threads..." << std::endl;
  stop_requested.store(true);

  stopStreamingThread();

  if (server_ptr) {
    server_ptr->stop();
    server_ptr = nullptr;
  }

  streaming_cv.notify_all();
}

int main(int argc, char *argv[]) {
  gst_init(&argc, &argv);

  bool preview_enabled_local = false;
  bool listen_enabled = false;
  bool send_enabled_mode = false;
  std::string listen_address = "";
  std::string server_ip = "127.0.0.1";
  int server_port = 12345;

  for (int i = 1; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--preview") {
      preview_enabled_local = true;
    } else if (arg == "--listen" && i + 1 < argc) {
      listen_enabled = true;
      listen_address = argv[++i];
    } else if (arg == "--send") {
      send_enabled_mode = true;
    } else if (arg == "--server" && i + 1 < argc) {
      server_ip = argv[++i];
    } else if (arg == "--port" && i + 1 < argc) {
      server_port = std::stoi(argv[++i]);
    } else if (arg == "--help") {
      std::cout << "Usage: " << argv[0] << " [options]\n";
      std::cout << "Options:\n";
      std::cout << "  --preview      Enable video preview\n";
      std::cout << "  --listen ADDR  Listen for control commands on address "
                   "(IP:PORT)\n";
      std::cout << "  --send         Send video stream directly to server\n";
      std::cout
          << "  --server IP    Server IP address (default: 127.0.0.1)\n";
      std::cout << "  --port PORT    Server port (default: 12345)\n";
      std::cout << "  --help         Show this help message\n";
      return 0;
    }
  }

  preview_enabled.store(preview_enabled_local);

  // Preview-only mode: simple pipeline, no threading needed
  if (preview_enabled_local && !send_enabled_mode && !listen_enabled) {
    g_unix_signal_add(
        SIGINT,
        [](gpointer user_data) -> gboolean {
          GMainLoop *l = static_cast<GMainLoop *>(user_data);
          g_main_loop_quit(l);
          return G_SOURCE_REMOVE;
        },
        nullptr); // Will set user_data after loop creation

    std::string pipeline_desc =
        "v4l2src device=/dev/video0 ! "
        "video/x-raw,width=1280,height=720 ! "
        "videoconvert ! ximagesink sync=false";

    g_print("Pipeline: %s\n", pipeline_desc.c_str());

    GError *error = nullptr;
    GstElement *pipeline = gst_parse_launch(pipeline_desc.c_str(), &error);
    if (!pipeline) {
      g_printerr("Failed to create pipeline: %s\n", error->message);
      g_clear_error(&error);
      return -1;
    }
    if (error) {
      g_printerr("Pipeline warning: %s\n", error->message);
      g_clear_error(&error);
    }

    GstStateChangeReturn ret =
        gst_element_set_state(pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
      g_printerr("Failed to set pipeline to PLAYING state\n");
      gst_object_unref(pipeline);
      return -1;
    }

    g_print("Preview started. Press Ctrl+C to stop.\n");

    loop = g_main_loop_new(nullptr, FALSE);

    // Re-register signal handler with loop as user_data
    g_unix_signal_add(
        SIGINT,
        [](gpointer user_data) -> gboolean {
          GMainLoop *l = static_cast<GMainLoop *>(user_data);
          g_main_loop_quit(l);
          return G_SOURCE_REMOVE;
        },
        loop);

    g_main_loop_run(loop);

    g_print("\nStopping pipeline...\n");
    gst_element_send_event(pipeline, gst_event_new_eos());
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    g_main_loop_unref(loop);
    loop = nullptr;
    return 0;
  }

  // --send or --listen modes use threaded approach
  if (!listen_enabled && !send_enabled_mode) {
    std::cerr << "Error: Either --listen, --send, or --preview option is "
                 "required"
              << std::endl;
    return -1;
  }

  signal(SIGINT, handle_sigint_threaded);

  if (send_enabled_mode) {
    if (server_ip.empty() || server_port == 0) {
      std::cerr
          << "Error: --send mode requires both --server and --port options"
          << std::endl;
      return -1;
    }

    send_to_server = server_ip;
    send_to_port = server_port;

    std::cout << "Starting direct video streaming to " << send_to_server << ":"
              << send_to_port << "..." << std::endl;

    {
      std::lock_guard<std::mutex> lock(config_mutex);
      current_camera_config.width = 1280;
      current_camera_config.height = 720;
      current_camera_config.fps = 30;
      current_camera_config.bitrate = 4000;
      current_camera_config.ip = send_to_server;
      current_camera_config.port = send_to_port;
    }

    startStreamingThread();

    std::cout << "Streaming started. Press Ctrl+C to stop." << std::endl;

    while (!stop_requested.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

  } else if (listen_enabled) {
    std::cout << "Starting webcam video streaming server..." << std::endl;

    listen_thread =
        make_unique_helper<std::thread>(listenThreadFunction, listen_address);

    std::cout << "Server started. Press Ctrl+C to stop." << std::endl;

    while (!stop_requested.load()) {
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    if (listen_thread && listen_thread->joinable()) {
      listen_thread->join();
    }
  }

  std::cout << "Shutting down..." << std::endl;
  stopStreamingThread();
  std::cout << "All threads stopped. Exiting." << std::endl;
  return 0;
}
