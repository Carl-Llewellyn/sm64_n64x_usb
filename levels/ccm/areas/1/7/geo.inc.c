// 0x0E00045C
const GeoLayout ccm_geo_00045C[] = {
    GEO_CULLING_RADIUS(900),
    GEO_OPEN_NODE(),
        GEO_RENDER_RANGE(-1000, 7000),
        GEO_OPEN_NODE(),
            GEO_DISPLAY_LIST(LAYER_OPAQUE, ccm_seg7_dl_0700F440),
            GEO_DISPLAY_LIST(LAYER_ALPHA, ccm_seg7_dl_0700F650),
            GEO_DISPLAY_LIST(LAYER_TRANSPARENT, ccm_seg7_dl_0700F780),
        GEO_CLOSE_NODE(),
    GEO_CLOSE_NODE(),
    GEO_END(),
};
