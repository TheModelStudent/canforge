// SPDX-License-Identifier: MIT
//
// The ftxui dashboard: a thin rendering layer over ViewModel.
//
// Frame rate. ftxui redraws on every event, so a naive implementation either
// spins at 100% of a core or updates visibly late. A dedicated ticker thread
// posts a custom event every 16 ms (about 62 Hz) and sleeps in between, so the
// process is idle between frames; measured cost with the simulator running is
// a few percent of one core. Resizing is ftxui's own business -- nothing here
// caches a width or a height, every layout is expressed in flex terms -- which
// is the whole reason not to hand-roll the drawing.

#include "canforge/tui/Dashboard.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#include "ftxui/component/component.hpp"
#include "ftxui/component/component_base.hpp"
#include "ftxui/component/screen_interactive.hpp"
#include "ftxui/dom/elements.hpp"
#include "ftxui/screen/color.hpp"

namespace canforge::tui {
namespace {

using namespace ftxui;  // NOLINT: ftxui's DSL is unusable when qualified

// Small fixed formatters, not one variadic helper: passing a runtime
// format string trips -Wformat-nonliteral, and the compiler can no longer
// check the argument types, which is exactly what that warning is for.
std::string f0(double v) {
  char b[48];
  std::snprintf(b, sizeof(b), "%.0f", v);
  return b;
}
std::string f1(double v) {
  char b[48];
  std::snprintf(b, sizeof(b), "%.1f", v);
  return b;
}
std::string f2(double v) {
  char b[48];
  std::snprintf(b, sizeof(b), "%.2f", v);
  return b;
}
std::string f4(double v) {
  char b[48];
  std::snprintf(b, sizeof(b), "%.4f", v);
  return b;
}
std::string fg(double v) {
  char b[48];
  std::snprintf(b, sizeof(b), "%g", v);
  return b;
}

std::string hex_id(core::CanId id) {
  char buffer[16];
  std::snprintf(buffer, sizeof(buffer), id.is_extended() ? "%08X" : "%03X", id.value());
  return buffer;
}

std::string hex_bytes(const std::array<std::uint8_t, 64>& data, std::size_t n) {
  static const char* digits = "0123456789ABCDEF";
  std::string out;
  for (std::size_t i = 0; i < n && i < 8; ++i) {
    if (i != 0) {
      out.push_back(' ');
    }
    out.push_back(digits[data[i] >> 4u]);
    out.push_back(digits[data[i] & 0x0Fu]);
  }
  if (n > 8) {
    out += " +";
    out += std::to_string(n - 8);
  }
  return out;
}

/// A stable colour per node, so the same ECU is the same colour every run.
Color node_colour(const std::string& node) {
  static const Color kPalette[] = {Color::Cyan,       Color::Green,    Color::Yellow,
                                   Color::Magenta,    Color::Blue,     Color::RedLight,
                                   Color::GreenLight, Color::CyanLight};
  std::size_t hash = 5381;
  for (const char c : node) {
    hash = hash * 33u + static_cast<unsigned char>(c);
  }
  return kPalette[hash % (sizeof(kPalette) / sizeof(kPalette[0]))];
}

const char* kSparkRows = " ▁▂▃▄▅▆▇█";

}  // namespace

struct Dashboard::Impl {
  ViewModel* model = nullptr;
  const core::Database* database = nullptr;
  TransmitFn transmit;
  ScreenInteractive screen = ScreenInteractive::Fullscreen();
  std::atomic<bool> running{true};

  int view = 0;  // 0..4
  bool help = false;
  std::string filter_text;
  std::string signal_text;
  std::string tx_message;
  std::string tx_values;
  std::string tx_status;
  int trace_scroll = 0;

  Element render_trace(const Snapshot& snap) const {
    Elements rows;
    const std::size_t visible = 200;
    const std::size_t begin =
        snap.trace.size() > visible ? snap.trace.size() - visible : 0;
    for (std::size_t i = begin; i < snap.trace.size(); ++i) {
      const TraceEntry& e = snap.trace[i];
      Elements line;
      line.push_back(text(f4(static_cast<double>(e.timestamp_ns) / 1e9) + " ") |
                     color(Color::GrayDark));
      line.push_back(text(hex_id(e.id) + "  ") |
                     color(e.is_error ? Color::Red : node_colour(e.node)));
      line.push_back(text("[" + std::to_string(e.size) + "] "));
      line.push_back(text(hex_bytes(e.data, e.size) + "  ") | color(Color::GrayLight));
      line.push_back(text(e.message_name.empty() ? "<unknown>" : e.message_name) |
                     bold | color(node_colour(e.node)));
      rows.push_back(hbox(std::move(line)));
      if (e.decoded) {
        Elements signals;
        for (std::size_t s = 0; s < e.signals.size(); ++s) {
          signals.push_back(text("  " + e.signals[s].first + "=") |
                            color(Color::GrayDark));
          signals.push_back(text(fg(e.signals[s].second)));
          if (!e.units[s].empty()) {
            signals.push_back(text(e.units[s]) | color(Color::GrayDark));
          }
        }
        rows.push_back(hbox(std::move(signals)) | color(Color::GrayLight));
      }
    }
    if (rows.empty()) {
      rows.push_back(text("waiting for traffic...") | color(Color::GrayDark));
    }
    return vbox(std::move(rows)) | focusPositionRelative(0, 1) | frame | flex;
  }

  Element render_grouped(const Snapshot& snap) const {
    Elements rows;
    rows.push_back(hbox({
                       text("ID      ") | bold,
                       text("MESSAGE               ") | bold,
                       text("NODE      ") | bold,
                       text("  COUNT ") | bold,
                       text(" CYCLE ") | bold,
                       text("ACTUAL ") | bold,
                       text("JITTER ") | bold,
                       text(" SIGNALS") | bold,
                   }) |
                   inverted);
    for (const MessageStats& m : snap.messages) {
      Elements line;
      line.push_back(text(hex_id(m.id) + std::string(8 - hex_id(m.id).size(), ' ')) |
                     color(node_colour(m.node)));
      std::string name = m.name;
      name.resize(22, ' ');
      line.push_back(text(name) | bold);
      std::string node = m.node;
      node.resize(10, ' ');
      line.push_back(text(node) | color(node_colour(m.node)));
      line.push_back(text(f0(static_cast<double>(m.count)) + " "));
      line.push_back(m.expected_cycle_ms != 0
                         ? text(f0(static_cast<double>(m.expected_cycle_ms)) + " ")
                         : (text("- ") | color(Color::GrayDark)));
      line.push_back(text(f1(m.mean_period_ms) + " ") |
                     color(m.period_suspect() ? Color::Red : Color::White));
      line.push_back(text(f2(m.jitter_ms) + " "));
      std::string signals;
      for (std::size_t i = 0; i < m.signals.size() && i < 4; ++i) {
        if (!signals.empty()) {
          signals += "  ";
        }
        signals += m.signals[i].first + "=" + fg(m.signals[i].second);
        if (i < m.units.size() && !m.units[i].empty()) {
          signals += m.units[i];
        }
      }
      line.push_back(text(" " + signals) | color(Color::GrayLight));
      rows.push_back(hbox(std::move(line)));
    }
    if (snap.messages.empty()) {
      rows.push_back(text("no messages seen yet") | color(Color::GrayDark));
    }
    return vbox(std::move(rows)) | frame | flex;
  }

  Element render_plot(const Snapshot& /*snap*/) const {
    const SignalHistory history = model->history();
    const std::string selected = model->selected_signal();
    Elements rows;
    rows.push_back(
        hbox({text("signal: ") | bold,
              text(selected.empty() ? "<none selected, press s>" : selected) |
                  color(Color::Cyan)}));
    if (history.empty()) {
      rows.push_back(text("no samples yet") | color(Color::GrayDark));
      return vbox(std::move(rows)) | flex;
    }
    rows.push_back(hbox({
        text("min " + fg(history.minimum()) + "   ") | color(Color::GrayDark),
        text("max " + fg(history.maximum()) + "   ") | color(Color::GrayDark),
        text("now " + fg(history.samples().back())) | bold,
    }));

    const std::vector<int> levels = history.levels(8);
    std::string spark;
    const std::size_t width = 160;
    const std::size_t begin = levels.size() > width ? levels.size() - width : 0;
    for (std::size_t i = begin; i < levels.size(); ++i) {
      const int level = std::clamp(levels[i], 0, 8);
      // The block characters are three bytes each in UTF-8.
      spark.append(kSparkRows + static_cast<std::size_t>(level) * 3, 3);
    }
    rows.push_back(text(spark) | color(Color::Green));
    rows.push_back(text(std::to_string(history.samples().size()) + " samples") |
                   color(Color::GrayDark));
    return vbox(std::move(rows)) | flex;
  }

  Element render_stats(const Snapshot& snap) const {
    const double load = snap.bus_load * 100.0;
    const int bar_width = 40;
    const int filled =
        std::clamp(static_cast<int>(load / 100.0 * bar_width), 0, bar_width);
    Elements rows;
    rows.push_back(hbox({
        text("bus load  ") | bold,
        text(std::string(static_cast<std::size_t>(filled), '#')) |
            color(load > 80.0   ? Color::Red
                  : load > 50.0 ? Color::Yellow
                                : Color::Green),
        text(std::string(static_cast<std::size_t>(bar_width - filled), '.')) |
            color(Color::GrayDark),
        text("  " + f1(load) + "%"),
    }));
    rows.push_back(text(""));
    rows.push_back(hbox({text("frames received  ") | bold,
                         text(std::to_string(snap.bus.frames_received))}));
    rows.push_back(hbox(
        {text("frames per second") | bold, text("  " + f0(snap.frames_per_second))}));
    rows.push_back(hbox({text("bytes received   ") | bold,
                         text(std::to_string(snap.bus.bytes_received))}));
    // Color's palette enumerators are distinct types, so a ternary over two of
    // them will not compile; the branch has to produce whole Elements.
    Element errors = text(std::to_string(snap.bus.error_frames));
    errors = snap.bus.error_frames != 0 ? (std::move(errors) | color(Color::Red))
                                        : (std::move(errors) | color(Color::White));
    rows.push_back(hbox({text("error frames     ") | bold, std::move(errors)}));
    rows.push_back(hbox({text("receive errors   ") | bold,
                         text(std::to_string(snap.bus.receive_errors))}));
    rows.push_back(hbox({text("dropped/filtered ") | bold,
                         text(std::to_string(snap.dropped_by_filter))}));
    rows.push_back(text(""));
    rows.push_back(text("per node") | bold | inverted);
    for (const NodeStats& n : snap.nodes) {
      const double share = snap.bus.wire_bits != 0
                               ? 100.0 * static_cast<double>(n.wire_bits) /
                                     static_cast<double>(snap.bus.wire_bits)
                               : 0.0;
      std::string name = n.name;
      name.resize(14, ' ');
      rows.push_back(hbox({
          text(name) | color(node_colour(n.name)),
          text(f0(static_cast<double>(n.frames)) + " frames  "),
          text(f0(static_cast<double>(n.bytes)) + " bytes  "),
          text(f1(share) + "% of the wire"),
      }));
    }
    return vbox(std::move(rows)) | flex;
  }

  Element render_transmit() const {  // NOLINT: symmetry with the others
    Elements rows;
    rows.push_back(text("compose a frame from physical signal values") | bold);
    rows.push_back(text(""));
    rows.push_back(hbox({text("message  ") | bold, text(tx_message) | inverted}));
    rows.push_back(hbox({text("values   ") | bold, text(tx_values) | inverted}));
    rows.push_back(text("  e.g. EngineSpeed=1500 ThrottlePosition=40") |
                   color(Color::GrayDark));
    rows.push_back(text(""));
    rows.push_back(text("enter  send once      p  send every 100 ms      x  stop") |
                   color(Color::GrayDark));
    rows.push_back(text(""));
    if (!tx_status.empty()) {
      rows.push_back(
          text(tx_status) |
          color(tx_status.rfind("sent", 0) == 0 ? Color::Green : Color::Red));
    }
    return vbox(std::move(rows)) | flex;
  }

  Element render_help() const {
    return vbox({
               text(" canforge dashboard ") | bold | inverted | center,
               text(""),
               text("  1   trace           scrolling frames, decoded"),
               text("  2   grouped         one row per message id"),
               text("  3   plot            sparkline for one signal"),
               text("  4   statistics      bus load, rates, per node"),
               text("  5   transmit        compose and send a frame"),
               text(""),
               text("  space  pause or resume the trace"),
               text("  /      edit the filter (message, id, node or signal)"),
               text("  s      choose the signal to plot"),
               text("  c      clear all statistics"),
               text("  ?      this help"),
               text("  q      quit"),
               text(""),
               text("  the filter freezes the trace only; statistics keep") |
                   color(Color::GrayDark),
               text("  counting, so bus load stays true while you read") |
                   color(Color::GrayDark),
           }) |
           border | center;
  }
};

Dashboard::Dashboard(ViewModel& model, const core::Database* database,
                     TransmitFn transmit)
    : impl_(std::make_unique<Impl>()) {
  impl_->model = &model;
  impl_->database = database;
  impl_->transmit = std::move(transmit);
}

Dashboard::~Dashboard() = default;

void Dashboard::stop() {
  impl_->running = false;
  impl_->screen.Exit();
}

int Dashboard::run() {
  Impl& s = *impl_;

  auto filter_input = Input(&s.filter_text, "filter");
  auto signal_input = Input(&s.signal_text, "Message.Signal");
  auto tx_message_input = Input(&s.tx_message, "message name");
  auto tx_values_input = Input(&s.tx_values, "Signal=value ...");
  auto container = Container::Vertical(
      {filter_input, signal_input, tx_message_input, tx_values_input});

  auto renderer = Renderer(container, [&] {
    const Snapshot snap = s.model->snapshot();

    Elements tabs;
    static const char* kNames[] = {"1 trace", "2 grouped", "3 plot", "4 statistics",
                                   "5 transmit"};
    for (int i = 0; i < 5; ++i) {
      tabs.push_back(i == s.view ? (text(std::string(" ") + kNames[i] + " ") | inverted)
                                 : (text(std::string(" ") + kNames[i] + " ") |
                                    color(Color::GrayDark)));
    }

    Element body;
    switch (s.view) {
      case 0:
        body = s.render_trace(snap);
        break;
      case 1:
        body = s.render_grouped(snap);
        break;
      case 2:
        body = s.render_plot(snap);
        break;
      case 3:
        body = s.render_stats(snap);
        break;
      default:
        body = s.render_transmit();
        break;
    }

    Elements status;
    Element mode = text(snap.paused ? " PAUSED " : " live ") | color(Color::Black);
    mode = snap.paused ? (std::move(mode) | bgcolor(Color::Red))
                       : (std::move(mode) | bgcolor(Color::Green));
    status.push_back(std::move(mode));
    status.push_back(text("  " + f0(snap.frames_per_second) + " fps"));
    status.push_back(text("  " + f1(snap.bus_load * 100.0) + "% load"));
    status.push_back(text("  " + std::to_string(snap.total_frames) + " frames"));
    status.push_back(filler());
    status.push_back(text("filter: "));
    status.push_back(filter_input->Render() | size(WIDTH, EQUAL, 18) | inverted);
    status.push_back(text("  ? help  q quit") | color(Color::GrayDark));

    Element page = vbox({
        hbox(std::move(tabs)),
        separator(),
        body,
        separator(),
        hbox(std::move(status)),
    });
    if (s.view == 2) {
      page =
          vbox({page, hbox({text("signal: "), signal_input->Render() |
                                                  size(WIDTH, EQUAL, 30) | inverted})});
    }
    if (s.view == 4) {
      page = vbox({page,
                   hbox({text("message: "), tx_message_input->Render() |
                                                size(WIDTH, EQUAL, 24) | inverted}),
                   hbox({text("values:  "), tx_values_input->Render() |
                                                size(WIDTH, EQUAL, 48) | inverted})});
    }
    if (s.help) {
      return dbox({page, s.render_help()});
    }
    return page;
  });

  auto with_keys = CatchEvent(renderer, [&](const Event& event) {
    // Text inputs get first refusal, so typing a "1" into the filter does not
    // switch views. Escape leaves the input.
    if (container->Focused() && event.is_character() && s.view != 0) {
      // fall through to the component
    }
    if (event == Event::Character('?')) {
      s.help = !s.help;
      return true;
    }
    if (s.help) {
      s.help = false;
      return true;
    }
    if (event == Event::Character('q') || event == Event::Escape) {
      if (event == Event::Escape && container->Focused()) {
        return false;
      }
      s.running = false;
      s.screen.Exit();
      return true;
    }
    for (int i = 0; i < 5; ++i) {
      if (event == Event::Character(static_cast<char>('1' + i)) &&
          !filter_input->Focused() && !signal_input->Focused() &&
          !tx_message_input->Focused() && !tx_values_input->Focused()) {
        s.view = i;
        return true;
      }
    }
    if (event == Event::Character(' ') && !container->Focused()) {
      s.model->set_paused(!s.model->paused());
      return true;
    }
    if (event == Event::Character('c') && !container->Focused()) {
      s.model->clear();
      return true;
    }
    if (event == Event::Return) {
      s.model->set_filter(s.filter_text);
      if (!s.signal_text.empty()) {
        s.model->select_signal(s.signal_text);
      }
      if (s.view == 4 && s.transmit) {
        const auto result = s.transmit(s.tx_message, s.tx_values, false);
        s.tx_status = result ? "sent " + s.tx_message
                             : "error: " + std::string(result.error().message());
      }
      return true;
    }
    if (event == Event::Character('p') && s.view == 4 && s.transmit) {
      const auto result = s.transmit(s.tx_message, s.tx_values, true);
      s.tx_status = result ? "sent " + s.tx_message + " periodically"
                           : "error: " + std::string(result.error().message());
      return true;
    }
    if (event == Event::Character('x') && s.view == 4 && s.transmit) {
      static_cast<void>(s.transmit("", "", false));
      s.tx_status = "periodic transmission stopped";
      return true;
    }
    return false;
  });

  // The frame clock. Sleeping between posts is what keeps the process off the
  // CPU: without it ftxui would redraw as fast as the terminal accepts bytes.
  std::thread ticker([&] {
    while (s.running) {
      std::this_thread::sleep_for(std::chrono::milliseconds(16));
      s.screen.PostEvent(Event::Custom);
    }
  });

  s.screen.Loop(with_keys);
  s.running = false;
  ticker.join();
  return 0;
}

}  // namespace canforge::tui
