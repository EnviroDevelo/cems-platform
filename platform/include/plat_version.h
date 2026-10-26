/* plat_version.h - the core platform's own version (Common Instrument Platform Specification
 * section 9, rule R4). Every product builds on one core version; it is shown next to the
 * product's own version on /api/info ("core"), the System screen and the event log's power-up
 * text, so a unit in the field says which core it carries.
 *
 * Semantic versioning, released as git tags core-MAJOR.MINOR.PATCH:
 *   PATCH  fixes only; every product builds and behaves the same
 *   MINOR  adds features; every product still builds and behaves the same
 *   MAJOR  may change the application contract (plat_app_t, param.h, plat_alarm.h, plat_log.h,
 *          ui_app.h, iobus.h) and comes with a migration note in CHANGELOG.md
 * The interface snapshots (docs/interface/) say when a change needs MINOR or MAJOR. */
#ifndef PLAT_VERSION_H
#define PLAT_VERSION_H

#define PLAT_CORE_MAJOR 0
#define PLAT_CORE_MINOR 9
#define PLAT_CORE_PATCH 0
#define PLAT_CORE_VERSION "0.9.0"      /* keep in step with the three numbers above */

#endif
