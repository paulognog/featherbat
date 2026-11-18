#include "drivers.h"
#include <hidsdi.h>
#include <hidpi.h>

static bool is_battery_usage(uint16_t page, USAGE usage) {
    return (page == 0x06 && usage == 0x20) || (page == 0x85 && usage == 0x65);
}

static bool has_digitizer(const HidList* hids, uint16_t vid, uint16_t pid) {
    for (int i = 0; i < hids->count; i++) {
        if (hids->items[i].vid == vid && hids->items[i].pid == pid && hids->items[i].usage_page == 0x0D) {
            return true;
        }
    }
    return false;
}

bool drv_genhid_present(const HidList* hids) {
    if (!hids) return false;
    for (int i = 0; i < hids->count; i++) {
        const HidColl* c = &hids->items[i];
        if (c->usage_page == 0x84 || c->usage_page == 0x85) continue;
        if (c->num_feat_val_caps || c->num_in_val_caps) {
            // Cheap filter: only worth scanning when a device with value caps isn't a plain mouse/kbd?
            // Actually many keyboards/mice have value caps; we check quickly on poll only if not claimed.
            return true;
        }
    }
    return false;
}

void drv_genhid_scan(ScanCtx* ctx) {
    if (!ctx->hids) return;
    for (int i = 0; i < ctx->hids->count; i++) {
        const HidColl* c = &ctx->hids->items[i];
        if (scan_ctx_is_claimed(ctx, c->vid, c->pid)) continue;
        if (c->usage_page == 0x84 || c->usage_page == 0x85) continue; // UPS / power supplies
        if (has_digitizer(ctx->hids, c->vid, c->pid)) continue;
        if (!c->num_feat_val_caps && !c->num_in_val_caps) continue;

        wchar_t cid[40];
        if (hid_container_id(c->path, cid, 40) && scan_ctx_has_bt_container(ctx, cid)) continue;

        HANDLE h = CreateFileW(c->path, 0, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
        if (h == INVALID_HANDLE_VALUE) continue;
        PHIDP_PREPARSED_DATA pp = NULL;
        if (!HidD_GetPreparsedData(h, &pp)) { CloseHandle(h); continue; }

        int pct = -1;
        HIDP_REPORT_TYPE types[2] = { HidP_Feature, HidP_Input };
        for (int t = 0; t < 2 && pct < 0; t++) {
            HIDP_REPORT_TYPE rtype = types[t];
            USHORT n = (rtype == HidP_Feature) ? c->num_feat_val_caps : c->num_in_val_caps;
            USHORT len = (rtype == HidP_Feature) ? c->feat_len : c->in_len;
            if (!n || !len || len > 512) continue;

            HIDP_VALUE_CAPS* vc = (HIDP_VALUE_CAPS*)xalloc(n * sizeof(HIDP_VALUE_CAPS));
            uint8_t* buf = (uint8_t*)xalloc(len);
            if (vc && buf && HidP_GetValueCaps(rtype, vc, &n, pp) == HIDP_STATUS_SUCCESS) {
                for (USHORT k = 0; k < n && pct < 0; k++) {
                    USAGE u = vc[k].IsRange ? vc[k].Range.UsageMin : vc[k].NotRange.Usage;
                    if (!is_battery_usage(vc[k].UsagePage, u)) continue;
                    memset(buf, 0, len);
                    buf[0] = vc[k].ReportID;
                    BOOLEAN got = (rtype == HidP_Feature)
                        ? HidD_GetFeature(h, buf, len)
                        : HidD_GetInputReport(h, buf, len);
                    if (!got) continue;
                    ULONG val = 0;
                    if (HidP_GetUsageValue(rtype, vc[k].UsagePage, vc[k].LinkCollection, u,
                                           &val, pp, (PCHAR)buf, len) != HIDP_STATUS_SUCCESS) continue;
                    if (c->vid == 0x05AC && val <= 100) {
                        pct = (int)val;
                    } else if (vc[k].LogicalMax > vc[k].LogicalMin) {
                        LONG span = vc[k].LogicalMax - vc[k].LogicalMin;
                        pct = clamp100((int)(((LONG)val - vc[k].LogicalMin) * 100 / span));
                    } else if (val <= 100) {
                        pct = (int)val;
                    }
                }
            }
            xfree(vc);
            xfree(buf);
        }

        HidD_FreePreparsedData(pp);
        CloseHandle(h);
        if (pct < 0) continue;

        Obs o;
        memset(&o, 0, sizeof(o));
        wfmt(o.id, 64, L"hid:%04x:%04x", c->vid, c->pid);
        hid_product_name(c->path, o.name, 96);
        if (!o.name[0]) wcpy(o.name, 96, L"Wireless device");
        wcpy(o.via, 32, L"HID");
        wcpy(o.container_id, 40, cid);
        o.kind = kind_from_hid(ctx->hids, c->vid, c->pid);
        if (o.kind == KIND_OTHER) o.kind = kind_from_name(o.name);
        o.has_kind = true;
        o.state = STATE_BATTERY;
        o.percent = pct;
        o.vid = c->vid;
        o.pid = c->pid;
        scan_ctx_add(ctx, &o);
        scan_ctx_claim(ctx, c->vid, c->pid);
    }
}
