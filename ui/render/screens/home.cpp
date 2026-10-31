#include "../../ui.h"
#include "screens.h"

// home: what this build can and can't do, said plainly

void fenrir::screens::home() {
    using namespace celosia;
    render::groupbox::begin("Fenrir", "Data recovery and erasure, without the overpromising");
    widgets::text("Recover opens a disk image read-only, finds deleted files on FAT32, exFAT and NTFS, carves JPEG, PNG,");
    widgets::text("PDF and ZIP files by their content, says how whole each one is and why, and exports them.");
    widgets::text("Every file it writes is new, and each export leaves a report with every file's SHA-256.");
    widgets::text("Erasure isn't in this build. No code in it can write to a drive.");
    widgets::text("Nothing leaves this computer: no telemetry, no uploads.");
    render::groupbox::end();

    render::groupbox::begin("Roadmap", "Each step lands with its tests, in this order");
    widgets::text("P1  read-only images: MBR and GPT, fault injection, SHA-256 before and after");
    widgets::text("P2  FAT32 deleted files, exported to a different drive");
    widgets::text("P3  file carving: JPEG, PNG, PDF and ZIP, checked structurally");
    widgets::text("P4  real scans in this window, on background workers");
    widgets::text("P5  NTFS and exFAT        P6  imaging with a map of unreadable sectors");
    widgets::text("P7  erasure simulated, then on disposable image files only");
    widgets::text("P8  real drives, after an independent review and a disposable hardware lab");
    render::groupbox::end();
}
