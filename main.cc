/* -*- c++ -*-
 * (c) h.zeller@acm.org. Free Software. GNU Public License v3.0 and above
 */

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <string>
#include <vector>

#include "rpt-parser.h"
#include "rpt2pnp.h"
#include "printer.h"
#include "postscript-printer.h"
#include "corner-part-collector.h"

static const float minimum_milliseconds = 50;
static const float area_to_milliseconds = 25;  // mm^2 to milliseconds.

// Smallest point from origin.
static float offset_x = 10;
static float offset_y = 10;

#define Z_DISPENSING "1.7"        // Position to dispense stuff. Just above board.
#define Z_HOVER_DISPENSER "2.5"   // Hovering above position.
#define Z_HIGH_UP_DISPENSER "5"   // high up to separate paste.

class GCodePrinter : public Printer {
public:
    GCodePrinter(float init_ms, float area_ms) : init_ms_(init_ms), area_ms_(area_ms) {}
    virtual void Init(float min_x, float min_y, float max_x, float max_y) {
        printf("; rpt2pnp -d %.2f -D %.2f file.rpt\n", init_ms_, area_ms_);
        // G-code preamble. Set feed rate, homing etc.
        printf(
               //    "G28\n" assume machine is already homed before g-code is executed
               "G21\n" // set to mm
               "G0 F20000\n"
               "G1 F4000\n"
               "G0 Z" Z_HIGH_UP_DISPENSER "\n"
               );
    }

    virtual void PrintPart(const Position &pos, const Part &part) {
        // move to new position, above board
        printf("G0 X%.3f Y%.3f E%.3f Z" Z_HOVER_DISPENSER " ; comp=%s val=%s\n",
               // "G1 Z" Z_HIGH_UP_DISPENSER "\n", // high above to have paste is well separated
               pos.x, pos.y, part.angle,
               part.component_name.c_str(), part.value.c_str());
    }

    virtual void Finish() {
        printf(";done\n");
    }

private:
    const float init_ms_;
    const float area_ms_;
};

class GCodeCornerIndicator : public Printer {
public:
    GCodeCornerIndicator(float init_ms, float area_ms) : init_ms_(init_ms), area_ms_(area_ms) {}

    virtual void Init(float min_x, float min_y, float max_x, float max_y) {
        corners_.SetCorners(min_x, min_y, max_x, max_y);
        // G-code preamble. Set feed rate, homing etc.
        printf(
               //    "G28\n" assume machine is already homed before g-code is executed
               "G21\n" // set to mm
               "G1 F2000\n"
               "G0 Z4\n" // X0 Y0 may be outside the reachable area, and no need to go there
               );
    }

    virtual void PrintPart(const Position &pos, const Part &part) {
        corners_.Update(pos, part);
    }

    virtual void Finish() {
        for (int i = 0; i < 4; ++i) {
            const ::Part &p = corners_.get_part(i);
            const Position pos = corners_.get_closest(i);
            printf("G0 X%.3f Y%.3f Z" Z_DISPENSING " ; comp=%s\n"
                   "G4 P2000 ; wtf\n"
                   "G0 Z" Z_HIGH_UP_DISPENSER "\n",
                   pos.x, pos.y, p.component_name.c_str()
                   );

        }
        printf(";done\n");
    }

private:
    CornerPartCollector corners_;
    const float init_ms_;
    const float area_ms_;
};

// Collect the parts from parse events.
class PartCollector : public ParseEventReceiver {
public:
    static void ReadRptFile(const std::string& rpt_file,
                            std::vector<const Part*> *result) {
        PartCollector collector(result);
        std::ifstream in(rpt_file);
        RptParse(&in, &collector);
    }

private:
    // Only to be used by public static functions.
    PartCollector(std::vector<const Part*> *parts)
        : origin_x_(0), origin_y_(0), current_part_(NULL),
          collected_parts_(parts) {}

protected:
    virtual void StartComponent(const std::string &c) {
        current_part_ = new Part();
        current_part_->component_name = c;
        drillSum = 0;
        angle_ = 0;
    }

    virtual void Value(const std::string &c) {
        current_part_->value = c;
    }

    virtual void EndComponent() {
        if (drillSum > 0)
            delete current_part_;  // through-hole. We're not interested in that.
        else
            collected_parts_->push_back(current_part_);
        current_part_ = NULL;
    }

    // Not caring about pads right now.
    virtual void StartPad(const std::string &c) {
        // collect pads for dispensing.
    }
    virtual void EndPad() { }

    virtual void Position(float x, float y) {
        // The first position callback we get is for the cmponent.
        if ((current_part_->pos.x == 0)
            && (current_part_->pos.y == 0)) {
            current_part_->pos.x = x;
            current_part_->pos.y = y;
        }
    }
    virtual void Size(float w, float h) {
        if (current_part_->dimension.w == 0
            && current_part_->dimension.h == 0) {
            current_part_->dimension.w = w;
            current_part_->dimension.h = h;
        }
    }

    virtual void Drill(float size) {
        drillSum += size; // looking for nonzero drill size
    }

    virtual void Orientation(float angle) {
        if (angle_ == 0) { // only take the first "position" of the component record
            // Angle is in degrees, make that radians.
            // mmh, and it looks like it turned in negative direction ? Probably part
            // of the mirroring.
            angle_ = -M_PI * angle / 180.0;
            current_part_->angle = angle; // change to _angle if you really want radians
        }
    }

private:
    void rotateXY(float *x, float *y) {
        float xnew = *x * cos(angle_) - *y * sin(angle_);
        float ynew = *x * sin(angle_) + *y * cos(angle_);
        *x = xnew;
        *y = ynew;
    }

    // Current coordinate system.
    float origin_x_;
    float origin_y_;
    float angle_;
    float drillSum; // add up all the pad drill sizes, should be 0 for smt

    std::string component_name_;
    Part *current_part_;
    std::vector<const Part*> *collected_parts_;
};

static int usage(const char *prog) {
    fprintf(stderr, "Usage: %s <options> <rpt-file>\n"
            "Options:\n"
            "\t-p      : Output as PostScript.\n"
            "\t-c      : Output corner DryRun G-Code.\n"
            "\t-d <ms> : Dispensing init time ms (default %.1f)\n"
            "\t-D <ms> : Dispensing time ms/mm^2 (default %.1f)\n",
            prog, minimum_milliseconds, area_to_milliseconds);
    return 1;
}

int main(int argc, char *argv[]) {
    enum OutputType {
        OUT_DISPENSING,
        OUT_CORNER_GCODE,
        OUT_POSTSCRIPT
    } output_type = OUT_DISPENSING;

    float start_ms = minimum_milliseconds;
    float area_ms = area_to_milliseconds;
    int opt;
    while ((opt = getopt(argc, argv, "pcd:D:")) != -1) {
        switch (opt) {
        case 'p':
            output_type = OUT_POSTSCRIPT;
            break;
        case 'c':
            output_type = OUT_CORNER_GCODE;
            break;
        case 'd':
            start_ms = atof(optarg);
            break;
        case 'D':
            area_ms = atof(optarg);
            break;
        default: /* '?' */
            return usage(argv[0]);
        }
    }

    if (optind >= argc) {
        return usage(argv[0]);
    }

    const char *rpt_file = argv[optind];

    std::vector<const Part*> parts;    
    PartCollector::ReadRptFile(rpt_file, &parts);

    // The coordinates coming out of the file are mirrored, so we determine the
    // maximum to mirror at these axes.
    // (mmh, looks like it is only mirrored on y axis ?)
    float min_x = parts[0]->pos.x, min_y = parts[0]->pos.y;
    float max_x = parts[0]->pos.x, max_y = parts[0]->pos.y;
    for (size_t i = 0; i < parts.size(); ++i) {
        min_x = std::min(min_x, parts[i]->pos.x);
        min_y = std::min(min_y, parts[i]->pos.y);
        max_x = std::max(max_x, parts[i]->pos.x);
        max_y = std::max(max_y, parts[i]->pos.y);
    }

    Printer *printer;
    switch (output_type) {
    case OUT_DISPENSING:   printer = new GCodePrinter(start_ms, area_ms); break;
    case OUT_CORNER_GCODE: printer = new GCodeCornerIndicator(start_ms, area_ms); break;
    case OUT_POSTSCRIPT:   printer = new PostScriptPrinter(); break;
    }

    OptimizeParts(&parts);

    printer->Init(offset_x, offset_y,
                  (max_x - min_x) + offset_x, (max_y - min_y) + offset_y);

    for (size_t i = 0; i < parts.size(); ++i) {
        const Part *part = parts[i];
        // We move x-coordinates relative to the smallest X.
        // Y-coordinates are mirrored at the maximum Y (that is how the come out of the file)
        printer->PrintPart(Position(part->pos.x + offset_x - min_x,
                                    max_y - part->pos.y + offset_y),
                           *part);
    }

    printer->Finish();

    fprintf(stderr, "Dispensed %zd parts. Total dispense time: %.1fs\n",
            parts.size(), 0.0f);
    for (size_t i = 0; i < parts.size(); ++i) {
        delete parts[i];
    }
    delete printer;
    return 0;
}
