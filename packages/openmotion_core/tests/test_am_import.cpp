// SPDX-License-Identifier: GPL-3.0-or-later
#include "openmotion/am_import.hpp"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <system_error>
#include <variant>
#include <vector>

#include <unistd.h>

#include <nlohmann/json.hpp>

#include "openmotion/media.hpp"
#include "openmotion/project.hpp"
#include "openmotion/renderer.hpp"

namespace {

namespace am_import = openmotion::am_import;
namespace project = openmotion::project;
namespace renderer = openmotion::renderer;

using project::BlendMode;
using project::Easing;
using project::Layer;
using project::LayerType;
using project::Project;

// Imports and requires success, returning the result.
am_import::ImportResult import_ok(const std::string& xml) {
    const std::variant<am_import::ImportResult, am_import::ImportError> result =
        am_import::import_am_xml(xml);
    if (const auto* error = std::get_if<am_import::ImportError>(&result)) {
        FAIL("import failed: " << error->message);
    }
    return std::get<am_import::ImportResult>(result);
}

// Imports and requires an error, returning its message.
std::string import_error(const std::string& xml) {
    const std::variant<am_import::ImportResult, am_import::ImportError> result =
        am_import::import_am_xml(xml);
    if (std::holds_alternative<am_import::ImportResult>(result)) {
        FAIL("expected an import error");
    }
    return std::get<am_import::ImportError>(result).message;
}

// True when the report holds an issue of `severity` whose message contains
// `needle`.
bool has_issue(const am_import::ImportReport& report, am_import::Issue::Severity severity,
               const std::string& needle) {
    for (const am_import::Issue& issue : report.issues) {
        if (issue.severity == severity && issue.message.find(needle) != std::string::npos) {
            return true;
        }
    }
    return false;
}

std::size_t count_issues(const am_import::ImportReport& report,
                         am_import::Issue::Severity severity) {
    std::size_t count = 0;
    for (const am_import::Issue& issue : report.issues) {
        if (issue.severity == severity) {
            ++count;
        }
    }
    return count;
}

bool command_available(const std::string& name) {
    const std::string command = "command -v " + name + " > /dev/null 2>&1";
    return std::system(command.c_str()) == 0;
}

}  // namespace

TEST_CASE("a minimal rect scene maps every field", "[am_import]") {
    const std::string xml = R"(<?xml version="1.0" encoding="utf-8"?>
<scene title="Hello" width="1280" height="720" fps="30" totalTime="3000">
  <shape id="1" label="box" s=".rect" startTime="100" endTime="3000" fillType="color">
    <fillColor value="#FFE04030"/>
    <property name="size" type="vec2" value="100,100"/>
    <transform>
      <property name="location" type="vec3" value="640,360,0"/>
      <property name="scale" type="vec2" value="9.745,9.745"/>
      <property name="rotation" type="float" value="45"/>
      <property name="opacity" type="float" value="0.75"/>
    </transform>
  </shape>
</scene>)";

    const am_import::ImportResult result = import_ok(xml);
    const Project& p = result.project;
    CHECK(p.version == 1);
    CHECK(p.name == "Hello");
    CHECK(p.width == 1280);
    CHECK(p.height == 720);
    CHECK(p.fps == Catch::Approx(30.0));
    CHECK(p.duration_ms == 3000);
    REQUIRE(p.layers.size() == 1);

    const Layer& layer = p.layers[0];
    CHECK(layer.id == "am_1");
    CHECK(layer.type == LayerType::Rect);
    CHECK(layer.name == "box");
    CHECK(layer.start_ms == 100);
    CHECK(layer.end_ms == 3000);
    CHECK(layer.color == "#E04030");
    CHECK(layer.width == Catch::Approx(200.0));   // 2 * half-extent
    CHECK(layer.height == Catch::Approx(200.0));
    CHECK(layer.transform.x.constant == Catch::Approx(640.0));
    CHECK(layer.transform.y.constant == Catch::Approx(360.0));
    CHECK(layer.transform.scale_x.constant == Catch::Approx(9.745));
    CHECK(layer.transform.scale_y.constant == Catch::Approx(9.745));
    CHECK(layer.transform.rotation.constant == Catch::Approx(45.0));
    CHECK(layer.transform.opacity.constant == Catch::Approx(0.75));
    CHECK(layer.blend == BlendMode::Normal);
}

TEST_CASE("a circle uses twice its size for width and height", "[am_import]") {
    const std::string xml = R"(<scene title="c" width="200" height="200" fps="24" totalTime="500">
  <shape id="7" label="dot" s=".circle" startTime="0" endTime="500" fillType="color">
    <fillColor value="#FF00FF00"/>
    <property name="size" type="vec2" value="50,30"/>
  </shape>
</scene>)";

    const am_import::ImportResult result = import_ok(xml);
    REQUIRE(result.project.layers.size() == 1);
    const Layer& layer = result.project.layers[0];
    CHECK(layer.type == LayerType::Circle);
    CHECK(layer.width == Catch::Approx(100.0));
    CHECK(layer.height == Catch::Approx(60.0));
}

TEST_CASE("keyframes are sorted, frame-mapped, and the easing is shifted", "[am_import]") {
    // A 10032 ms layer at 30 fps. The keyframes are deliberately out of order.
    const std::string xml = R"(<scene title="k" width="1280" height="720" fps="30" totalTime="10032">
  <shape id="3" label="mover" s=".rect" startTime="0" endTime="10032" fillType="color">
    <fillColor value="#FFFFFFFF"/>
    <property name="size" value="10,10"/>
    <transform>
      <property name="location" type="vec3">
        <kf t="0.5" v="500,0,0" e="cubicBezier 0.2 0.8 0.4 1.0"/>
        <kf t="0.300638" v="200,0,0" e="linear"/>
        <kf t="0.8" v="800,0,0" e="elastic 1 2 3 4"/>
        <kf t="-0.0039" v="0,0,0" e="linear"/>
      </property>
    </transform>
  </shape>
</scene>)";

    const am_import::ImportResult result = import_ok(xml);
    REQUIRE(result.project.layers.size() == 1);
    const std::vector<project::Keyframe>& kfs = result.project.layers[0].transform.x.keyframes;
    REQUIRE(kfs.size() == 4);

    // Sorted by t: -0.0039, 0.300638, 0.5, 0.8.
    CHECK(kfs[0].time_ms == 0);  // clamped from a negative t
    CHECK(kfs[0].value == Catch::Approx(0.0));
    CHECK(kfs[0].easing == Easing::Linear);  // easing of sorted[1] == "linear"

    CHECK(kfs[1].time_ms == 3000);  // t = 0.300638 -> frame 90
    CHECK(kfs[1].value == Catch::Approx(200.0));
    // kf[1]'s easing comes from the next sorted keyframe (t = 0.5).
    CHECK(kfs[1].easing == Easing::CubicBezier);
    CHECK(kfs[1].bezier[0] == Catch::Approx(0.2));
    CHECK(kfs[1].bezier[1] == Catch::Approx(0.8));
    CHECK(kfs[1].bezier[2] == Catch::Approx(0.4));
    CHECK(kfs[1].bezier[3] == Catch::Approx(1.0));

    CHECK(kfs[2].time_ms == 5000);  // t = 0.5 -> frame 150
    CHECK(kfs[2].value == Catch::Approx(500.0));
    // kf[2]'s easing comes from the next sorted keyframe (t = 0.8, "elastic").
    CHECK(kfs[2].easing == Easing::Linear);

    CHECK(kfs[3].time_ms == 8000);  // t = 0.8 -> frame 240
    CHECK(kfs[3].value == Catch::Approx(800.0));
    CHECK(kfs[3].easing == Easing::Linear);  // the last keyframe is linear

    // The elastic easing on the arriving keyframe produced a warning.
    CHECK(has_issue(result.report, am_import::Issue::Severity::Warning, "elastic"));
}

TEST_CASE("the frame formula rounds to the nearest frame and clamps at zero", "[am_import]") {
    const std::string xml = R"(<scene title="f" width="64" height="64" fps="30" totalTime="10032">
  <shape id="1" label="a" s=".rect" startTime="0" endTime="10032" fillType="color">
    <fillColor value="#FFFFFFFF"/>
    <property name="size" value="1,1"/>
    <transform>
      <property name="rotation" type="float">
        <kf t="-0.0039" v="0" e="linear"/>
        <kf t="0.300638" v="90" e="linear"/>
      </property>
    </transform>
  </shape>
</scene>)";

    const am_import::ImportResult result = import_ok(xml);
    const std::vector<project::Keyframe>& kfs =
        result.project.layers[0].transform.rotation.keyframes;
    REQUIRE(kfs.size() == 2);
    CHECK(kfs[0].time_ms == 0);     // negative t clamped to frame 0
    CHECK(kfs[1].time_ms == 3000);  // frame 90 at 30 fps
}

TEST_CASE("out-of-range and unknown easings fall back to linear with a warning",
          "[am_import]") {
    const std::string xml = R"(<scene title="e" width="64" height="64" fps="30" totalTime="1000">
  <shape id="1" label="a" s=".rect" startTime="0" endTime="1000" fillType="color">
    <fillColor value="#FFFFFFFF"/>
    <property name="size" value="1,1"/>
    <transform>
      <property name="opacity" type="float">
        <kf t="0.0" v="1.0" e="linear"/>
        <kf t="0.5" v="0.5" e="cubicBezier 1.5 0.0 0.4 1.0"/>
        <kf t="1.0" v="0.0" e="bogus"/>
      </property>
    </transform>
  </shape>
</scene>)";

    const am_import::ImportResult result = import_ok(xml);
    const std::vector<project::Keyframe>& kfs =
        result.project.layers[0].transform.opacity.keyframes;
    REQUIRE(kfs.size() == 3);
    // kf0's easing comes from the next keyframe's out-of-range cubicBezier.
    CHECK(kfs[0].easing == Easing::Linear);
    // kf1's easing comes from the last keyframe's unknown string.
    CHECK(kfs[1].easing == Easing::Linear);
    CHECK(kfs[2].easing == Easing::Linear);
    CHECK(has_issue(result.report, am_import::Issue::Severity::Warning, "out of range"));
    CHECK(has_issue(result.report, am_import::Issue::Severity::Warning, "unknown easing"));
}

TEST_CASE("two keyframes on one frame keep the last and warn", "[am_import]") {
    const std::string xml = R"(<scene title="d" width="64" height="64" fps="30" totalTime="10032">
  <shape id="1" label="a" s=".rect" startTime="0" endTime="10032" fillType="color">
    <fillColor value="#FFFFFFFF"/>
    <property name="size" value="1,1"/>
    <transform>
      <property name="opacity" type="float">
        <kf t="0.5" v="0.25" e="linear"/>
        <kf t="0.50001" v="0.75" e="linear"/>
        <kf t="1.0" v="1.0" e="linear"/>
      </property>
    </transform>
  </shape>
</scene>)";

    const am_import::ImportResult result = import_ok(xml);
    const std::vector<project::Keyframe>& kfs =
        result.project.layers[0].transform.opacity.keyframes;
    REQUIRE(kfs.size() == 2);
    // Both t = 0.5 and t = 0.50001 land on frame 150; the last wins.
    CHECK(kfs[0].time_ms == 5000);
    CHECK(kfs[0].value == Catch::Approx(0.75));
    CHECK(has_issue(result.report, am_import::Issue::Severity::Warning, "same frame"));
}

TEST_CASE("fill alpha multiplies into opacity for constants and keyframes", "[am_import]") {
    const std::string constant_xml =
        R"(<scene title="a" width="64" height="64" fps="30" totalTime="1000">
  <shape id="1" label="a" s=".rect" startTime="0" endTime="1000" fillType="color">
    <fillColor value="#80FF0000"/>
    <property name="size" value="1,1"/>
    <transform><property name="opacity" type="float" value="0.5"/></transform>
  </shape>
</scene>)";
    const am_import::ImportResult constant = import_ok(constant_xml);
    // 0.5 * (128 / 255).
    CHECK(constant.project.layers[0].transform.opacity.constant ==
          Catch::Approx(0.5 * 128.0 / 255.0));

    const std::string keyframed_xml =
        R"(<scene title="a" width="64" height="64" fps="30" totalTime="1000">
  <shape id="1" label="a" s=".rect" startTime="0" endTime="1000" fillType="color">
    <fillColor value="#80FF0000"/>
    <property name="size" value="1,1"/>
    <transform>
      <property name="opacity" type="float">
        <kf t="0.0" v="1.0" e="linear"/>
        <kf t="1.0" v="0.2" e="linear"/>
      </property>
    </transform>
  </shape>
</scene>)";
    const am_import::ImportResult keyframed = import_ok(keyframed_xml);
    const std::vector<project::Keyframe>& kfs =
        keyframed.project.layers[0].transform.opacity.keyframes;
    REQUIRE(kfs.size() == 2);
    CHECK(kfs[0].value == Catch::Approx(1.0 * 128.0 / 255.0));
    CHECK(kfs[1].value == Catch::Approx(0.2 * 128.0 / 255.0));
}

TEST_CASE("every supported blend name maps and unknown ones warn", "[am_import]") {
    const struct {
        const char* name;
        BlendMode mode;
    } cases[] = {
        {"screen", BlendMode::Screen},
        {"multiply", BlendMode::Multiply},
        {"exclude", BlendMode::Exclusion},
        {"color-burn", BlendMode::ColorBurn},
        {"linear-dodge", BlendMode::LinearDodge},
        {"none", BlendMode::Normal},
    };
    for (const auto& test : cases) {
        const std::string xml = std::string(
            R"(<scene title="b" width="64" height="64" fps="30" totalTime="1000">
  <shape id="1" label="a" s=".rect" startTime="0" endTime="1000" fillType="color" blending=")") +
            test.name + R"(">
    <fillColor value="#FFFFFFFF"/>
    <property name="size" value="1,1"/>
  </shape>
</scene>)";
        INFO("blending " << test.name);
        const am_import::ImportResult result = import_ok(xml);
        REQUIRE(result.project.layers.size() == 1);
        CHECK(result.project.layers[0].blend == test.mode);
        CHECK(count_issues(result.report, am_import::Issue::Severity::Warning) == 0);
    }

    const std::string unknown = R"(<scene title="b" width="64" height="64" fps="30" totalTime="1000">
  <shape id="1" label="a" s=".rect" startTime="0" endTime="1000" fillType="color" blending="foo">
    <fillColor value="#FFFFFFFF"/>
    <property name="size" value="1,1"/>
  </shape>
</scene>)";
    const am_import::ImportResult unknown_result = import_ok(unknown);
    CHECK(unknown_result.project.layers[0].blend == BlendMode::Normal);
    CHECK(has_issue(unknown_result.report, am_import::Issue::Severity::Warning, "unknown blend"));

    const std::string mask = R"(<scene title="b" width="64" height="64" fps="30" totalTime="1000">
  <shape id="1" label="a" s=".rect" startTime="0" endTime="1000" fillType="color" blending="mask">
    <fillColor value="#FFFFFFFF"/>
    <property name="size" value="1,1"/>
  </shape>
</scene>)";
    const am_import::ImportResult mask_result = import_ok(mask);
    CHECK(mask_result.project.layers[0].blend == BlendMode::Normal);
    CHECK(has_issue(mask_result.report, am_import::Issue::Severity::Warning, "mask"));
}

TEST_CASE("unsupported content is reported and the rest still imports", "[am_import]") {
    const std::string xml = R"(<scene title="mix" width="1280" height="720" fps="30" totalTime="1000">
  <shape id="1" label="keep" s=".rect" startTime="0" endTime="1000" fillType="color">
    <fillColor value="#FF102030"/>
    <property name="size" value="10,10"/>
    <effect id="com.alightcreative.effects.gaussianblur"/>
    <effect id="com.alightcreative.effects.invert"/>
  </shape>
  <shape id="2" label="gone" s=".rect" startTime="0" endTime="1000" fillType="color" hidden="true">
    <fillColor value="#FFFFFFFF"/>
    <property name="size" value="10,10"/>
  </shape>
  <text id="3" label="words" startTime="0" endTime="1000"><content>hi</content></text>
  <nullobj id="4" label="null" type="perspective" startTime="0" endTime="1000"/>
  <shape id="5" label="line" s=".line" startTime="0" endTime="1000" fillType="color">
    <property name="size" value="10,10"/>
  </shape>
  <shape id="6" label="path" s=".rect" startTime="0" endTime="1000" fillType="color">
    <path d="M 0 0 L 10 0 Z"/>
    <property name="size" value="10,10"/>
  </shape>
  <embedScene id="7" label="precomp" startTime="0" endTime="1000" fillType="intrinsic">
    <scene title="nested" width="100" height="100" fps="30" totalTime="1000">
      <shape id="8" s=".rect"><property name="size" value="1,1"/></shape>
      <shape id="9" s=".rect"><property name="size" value="1,1"/></shape>
    </scene>
  </embedScene>
  <bookmark t="500"/>
  <bookmark t="900"/>
</scene>)";

    const am_import::ImportResult result = import_ok(xml);
    // Only the first, supported layer is imported.
    REQUIRE(result.project.layers.size() == 1);
    CHECK(result.project.layers[0].name == "keep");

    CHECK(has_issue(result.report, am_import::Issue::Severity::Skipped, "hidden"));
    CHECK(has_issue(result.report, am_import::Issue::Severity::Skipped, "text"));
    CHECK(has_issue(result.report, am_import::Issue::Severity::Skipped, "null object"));
    CHECK(has_issue(result.report, am_import::Issue::Severity::Skipped, "not supported"));
    CHECK(has_issue(result.report, am_import::Issue::Severity::Skipped, "nested composition"));
    CHECK(has_issue(result.report, am_import::Issue::Severity::Skipped, "2 layer"));
    // Two effects plus one bookmark issue.
    CHECK(count_issues(result.report, am_import::Issue::Severity::Info) == 3);
    CHECK(has_issue(result.report, am_import::Issue::Severity::Info, "gaussianblur"));
    CHECK(has_issue(result.report, am_import::Issue::Severity::Info, "2 bookmark"));
}

TEST_CASE("media, gradient and round_rect fills are converted best-effort", "[am_import]") {
    const std::string xml = R"(<scene title="fills" width="1280" height="720" fps="30" totalTime="1000">
  <shape id="1" label="pic.png" s=".rect" startTime="0" endTime="1000" fillType="media">
    <property name="size" value="10,10"/>
  </shape>
  <shape id="2" label="grad" s=".rect" startTime="0" endTime="1000" fillType="gradient">
    <gradient type="linear" startColor="#FF00FF00" endColor="#FFFF0000" start="0,0" end="1,1"/>
    <property name="size" value="10,10"/>
  </shape>
  <shape id="3" label="rounded" s=".roundrect" startTime="0" endTime="1000" fillType="color">
    <fillColor value="#FF123456"/>
    <property name="size" value="10,10"/>
  </shape>
</scene>)";

    const am_import::ImportResult result = import_ok(xml);
    REQUIRE(result.project.layers.size() == 3);

    CHECK(result.project.layers[0].type == LayerType::Image);
    CHECK(result.project.layers[0].source == "pic.png");

    CHECK(result.project.layers[1].type == LayerType::Rect);
    CHECK(result.project.layers[1].color == "#00FF00");  // the start colour

    CHECK(result.project.layers[2].type == LayerType::RoundRect);
    CHECK(result.project.layers[2].radius == Catch::Approx(0.0));

    CHECK(has_issue(result.report, am_import::Issue::Severity::Warning, "gradient geometry"));
    CHECK(has_issue(result.report, am_import::Issue::Severity::Warning, "corner radius"));
    CHECK(has_issue(result.report, am_import::Issue::Severity::Warning, "mediaFillMode"));
}

TEST_CASE("parent and pivot produce warnings", "[am_import]") {
    const std::string xml = R"(<scene title="p" width="64" height="64" fps="30" totalTime="1000">
  <shape id="1" label="child" s=".rect" startTime="0" endTime="1000" fillType="color" parent="0">
    <fillColor value="#FFFFFFFF"/>
    <property name="size" value="10,10"/>
    <transform><property name="pivot" type="vec2" value="10,20"/></transform>
  </shape>
</scene>)";

    const am_import::ImportResult result = import_ok(xml);
    CHECK(has_issue(result.report, am_import::Issue::Severity::Warning, "parent transforms"));
    CHECK(has_issue(result.report, am_import::Issue::Severity::Warning, "pivot ignored"));
}

TEST_CASE("a skipped layer does not abort the import", "[am_import]") {
    const std::string xml = R"(<scene title="s" width="64" height="64" fps="30" totalTime="1000">
  <shape id="1" label="bad" s=".rect" startTime="500" endTime="100" fillType="color">
    <fillColor value="#FFFFFFFF"/>
    <property name="size" value="10,10"/>
  </shape>
  <shape id="2" label="good" s=".rect" startTime="0" endTime="1000" fillType="color">
    <fillColor value="#FFFFFFFF"/>
    <property name="size" value="10,10"/>
  </shape>
</scene>)";

    const am_import::ImportResult result = import_ok(xml);
    REQUIRE(result.project.layers.size() == 1);
    CHECK(result.project.layers[0].name == "good");
    CHECK(has_issue(result.report, am_import::Issue::Severity::Skipped, "before start"));
}

TEST_CASE("layer ids are made unique", "[am_import]") {
    const std::string xml = R"(<scene title="u" width="64" height="64" fps="30" totalTime="1000">
  <shape id="5" label="a" s=".rect" startTime="0" endTime="1000" fillType="color">
    <fillColor value="#FFFFFFFF"/><property name="size" value="1,1"/>
  </shape>
  <shape id="5" label="b" s=".rect" startTime="0" endTime="1000" fillType="color">
    <fillColor value="#FFFFFFFF"/><property name="size" value="1,1"/>
  </shape>
</scene>)";

    const am_import::ImportResult result = import_ok(xml);
    REQUIRE(result.project.layers.size() == 2);
    CHECK(result.project.layers[0].id == "am_5");
    CHECK(result.project.layers[1].id != result.project.layers[0].id);
}

TEST_CASE("scene-level errors are reported", "[am_import]") {
    CHECK(import_error("<<< not xml >>>").find("parse") != std::string::npos);
    CHECK(import_error("<notscene/>").find("root element") != std::string::npos);
    CHECK(import_error(R"(<scene height="720" fps="30" totalTime="0"/>)").find("width") !=
          std::string::npos);
    CHECK(import_error(R"(<scene width="0" height="720" fps="30" totalTime="0"/>)").find("width") !=
          std::string::npos);
    CHECK(import_error(R"(<scene width="20000" height="720" fps="30" totalTime="0"/>)")
              .find("width") != std::string::npos);
    CHECK(import_error(R"(<scene width="1280" height="720" fps="0" totalTime="0"/>)").find("fps") !=
          std::string::npos);
    CHECK(import_error(R"(<scene width="1280" height="720" fps="30" totalTime="-5"/>)")
              .find("totalTime") != std::string::npos);
    // 1e400 parses as infinity and must be rejected, not converted.
    CHECK(import_error(R"(<scene width="1e400" height="720" fps="30" totalTime="0"/>)")
              .find("width") != std::string::npos);
    CHECK(import_error(R"(<scene width="1280" height="720" fps="nan" totalTime="0"/>)").find("fps") !=
          std::string::npos);
}

TEST_CASE("a non-finite layer number is an error", "[am_import]") {
    const std::string xml = R"(<scene title="n" width="64" height="64" fps="30" totalTime="1000">
  <shape id="1" label="a" s=".rect" startTime="0" endTime="1000" fillType="color">
    <fillColor value="#FFFFFFFF"/>
    <property name="size" value="1,1"/>
    <transform><property name="location" value="1e400,0,0"/></transform>
  </shape>
</scene>)";
    CHECK(import_error(xml).find("non-finite") != std::string::npos);
}

TEST_CASE("the layer and keyframe limits are enforced", "[am_import]") {
    std::string too_many = R"(<scene title="big" width="64" height="64" fps="30" totalTime="1000">)";
    for (std::size_t i = 0; i <= am_import::kMaxLayers; ++i) {
        too_many +=
            R"(<shape id=")" + std::to_string(i) +
            R"(" s=".rect" startTime="0" endTime="1000" fillType="color"><fillColor value="#FFFFFFFF"/><property name="size" value="1,1"/></shape>)";
    }
    too_many += "</scene>";
    CHECK(import_error(too_many).find("layers") != std::string::npos);

    std::string too_many_kfs =
        R"(<scene title="k" width="64" height="64" fps="30" totalTime="1000">
  <shape id="1" label="a" s=".rect" startTime="0" endTime="1000" fillType="color">
    <fillColor value="#FFFFFFFF"/><property name="size" value="1,1"/>
    <transform><property name="opacity" type="float">)";
    for (std::size_t i = 0; i <= am_import::kMaxKeyframesPerProperty; ++i) {
        too_many_kfs += R"(<kf t="0.5" v="1.0" e="linear"/>)";
    }
    too_many_kfs += R"(</property></transform></shape></scene>)";
    CHECK(import_error(too_many_kfs).find("keyframes") != std::string::npos);
}

TEST_CASE("the input size limit is checked without allocating it", "[am_import]") {
    CHECK(am_import::input_size_ok(0));
    CHECK(am_import::input_size_ok(am_import::kMaxInputBytes));
    CHECK_FALSE(am_import::input_size_ok(am_import::kMaxInputBytes + 1));
    CHECK(am_import::input_size_ok(10, 10));
    CHECK_FALSE(am_import::input_size_ok(11, 10));
}

TEST_CASE("the produced project survives a serialize and reload", "[am_import]") {
    const std::string xml = R"(<scene title="rt" width="320" height="240" fps="25" totalTime="2000">
  <shape id="1" label="a" s=".circle" startTime="0" endTime="2000" fillType="color" blending="screen">
    <fillColor value="#FF808080"/>
    <property name="size" value="20,20"/>
    <transform>
      <property name="location" type="vec3" value="160,120,0"/>
      <property name="opacity" type="float">
        <kf t="0.0" v="0.0" e="linear"/>
        <kf t="1.0" v="1.0" e="linear"/>
      </property>
    </transform>
  </shape>
</scene>)";

    const am_import::ImportResult result = import_ok(xml);
    const std::string encoded = project::to_json_string(result.project);
    const std::variant<Project, project::ProjectError> reloaded =
        project::load_from_string(encoded);
    REQUIRE(std::holds_alternative<Project>(reloaded));
    const Project& round = std::get<Project>(reloaded);
    CHECK(round.name == "rt");
    CHECK(round.width == 320);
    REQUIRE(round.layers.size() == 1);
    CHECK(round.layers[0].blend == BlendMode::Screen);
    REQUIRE(round.layers[0].transform.opacity.keyframes.size() == 2);
}

TEST_CASE("report_to_json is valid JSON with severities and messages", "[am_import]") {
    const std::string xml = R"(<scene title="j" width="64" height="64" fps="30" totalTime="1000">
  <shape id="1" label="a" s=".rect" startTime="0" endTime="1000" fillType="color" blending="foo">
    <fillColor value="#FFFFFFFF"/><property name="size" value="1,1"/>
  </shape>
  <text id="2" label="t" startTime="0" endTime="1000"><content>x</content></text>
</scene>)";

    const am_import::ImportResult result = import_ok(xml);
    const std::string text = am_import::report_to_json(result.report);

    nlohmann::json parsed;
    REQUIRE_NOTHROW(parsed = nlohmann::json::parse(text));
    REQUIRE(parsed.contains("issues"));
    REQUIRE(parsed["issues"].is_array());

    bool saw_warning = false;
    bool saw_skipped = false;
    for (const nlohmann::json& issue : parsed["issues"]) {
        CHECK(issue.contains("severity"));
        CHECK(issue.contains("layer"));
        CHECK(issue.contains("message"));
        if (issue["severity"] == "warning") {
            saw_warning = true;
        }
        if (issue["severity"] == "skipped") {
            saw_skipped = true;
        }
    }
    CHECK(saw_warning);
    CHECK(saw_skipped);
}

TEST_CASE("import_am_file reads a file and never throws on a missing one", "[am_import]") {
    const std::filesystem::path path =
        std::filesystem::temp_directory_path() /
        ("openmotion_am_" + std::to_string(::getpid()) + ".xml");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << R"(<scene title="file" width="64" height="64" fps="30" totalTime="0"/>)";
    }
    const std::variant<am_import::ImportResult, am_import::ImportError> imported =
        am_import::import_am_file(path.string());
    std::error_code ignored;
    std::filesystem::remove(path, ignored);

    REQUIRE(std::holds_alternative<am_import::ImportResult>(imported));
    CHECK(std::get<am_import::ImportResult>(imported).project.name == "file");

    const std::variant<am_import::ImportResult, am_import::ImportError> missing =
        am_import::import_am_file(path.string() + ".nope");
    CHECK(std::holds_alternative<am_import::ImportError>(missing));
}

TEST_CASE("end to end: import, save and render an AM scene", "[am_import]") {
    if (!command_available("ffmpeg") || !command_available("ffprobe")) {
        SKIP("ffmpeg/ffprobe are not available");
    }

    const std::string xml = R"(<?xml version="1.0" encoding="utf-8"?>
<!-- Exported by Alight Motion 5.0.275 -->
<scene title="End to end" width="128" height="96" fps="30" totalTime="1000" bgcolor="#00000000">
  <shape id="1" label="bg" s=".rect" startTime="0" endTime="1000" fillType="color" blending="none">
    <fillColor value="#FF204060"/>
    <property name="size" type="vec2" value="64,48"/>
    <transform><property name="location" type="vec3" value="64,48,0"/></transform>
  </shape>
  <shape id="2" label="dot" s=".circle" startTime="0" endTime="1000" fillType="color" blending="screen">
    <fillColor value="#FFFF8000"/>
    <property name="size" type="vec2" value="20,20"/>
    <transform>
      <property name="location" type="vec3" value="64,48,0"/>
      <property name="opacity" type="float" value="0.8"/>
    </transform>
    <effect id="com.alightcreative.effects.glow"/>
  </shape>
  <bookmark t="500"/>
</scene>)";

    const am_import::ImportResult result = import_ok(xml);

    // The report for this exact input is pasted in the task's final report.
    std::fputs(am_import::report_to_json(result.report).c_str(), stdout);
    std::fputc('\n', stdout);

    const std::filesystem::path base =
        std::filesystem::temp_directory_path() /
        ("openmotion_am_e2e_" + std::to_string(::getpid()));
    const std::filesystem::path proj_path = base.string() + ".omproj";
    const std::filesystem::path clip_path = base.string() + ".mp4";

    REQUIRE_FALSE(project::save_to_file(result.project, proj_path.string()).has_value());

    const std::optional<renderer::RenderError> render_error =
        renderer::render_to_mp4(result.project, clip_path.string());
    REQUIRE_FALSE(render_error.has_value());

    const std::variant<openmotion::media::VideoMetadata, openmotion::media::MediaError> metadata =
        openmotion::media::open_video(clip_path.string());

    std::error_code ignored;
    std::filesystem::remove(proj_path, ignored);
    std::filesystem::remove(clip_path, ignored);

    REQUIRE(std::holds_alternative<openmotion::media::VideoMetadata>(metadata));
    const openmotion::media::VideoMetadata& info =
        std::get<openmotion::media::VideoMetadata>(metadata);
    CHECK(info.width == 128);
    CHECK(info.height == 96);
    CHECK(info.duration_ms > 700);
    CHECK(info.duration_ms < 1300);
}
