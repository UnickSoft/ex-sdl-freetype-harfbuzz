#include <stdlib.h>
#include <stdio.h>
#include <assert.h>

#include <SDL2/SDL.h>
#include <SDL2/SDL_render.h>
#undef main

#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_GLYPH_H
#include FT_OUTLINE_H

#include <hb.h>
#include <hb-ot.h>
#include <string>

#include <vector>

#include <fribidi.h>

#ifdef _WIN32
	#include <hb-ot-font.h>
	#include <Windows.h>
#endif

// Lazzy fix for Release for VS
#ifndef __FULLPATH_FILE__
	#define __FULLPATH_FILE__ __FILE__
#endif

namespace Example
{
#ifdef _WIN32
	typedef unsigned int uint;
#endif

	const wchar_t *text = L"Ленивый рыжий кот شَدَّة latin العَرَبِية";
    
    typedef struct _spanner_baton_t {
        /* rendering part - assumes 32bpp surface */
        uint32_t *pixels; // set to the glyph's origin.
        uint32_t *first_pixel, *last_pixel; // bounds check
        uint32_t pitch;
        uint32_t rshift;
        uint32_t gshift;
        uint32_t bshift;
        uint32_t ashift;
        
        /* sizing part */
        int min_span_x;
        int max_span_x;
        int min_y;
        int max_y;
    } spanner_baton_t;
    
    /* google this */
#ifndef unlikely
#define unlikely
#endif
    
    /* This spanner is write only, suitable for write-only mapped buffers,
     but can cause dark streaks where glyphs overlap, like in arabic scripts.
     
     Note how spanners don't clip against surface width - resize the window
     and see what it leads to. */
    void spanner_wo(int y, int count, const FT_Span* spans, void *user) {
        spanner_baton_t *baton = (spanner_baton_t *) user;
        uint32_t *scanline = baton->pixels - y * ( (int) baton->pitch / 4 );
        if (unlikely scanline < baton->first_pixel)
            return;
        for (int i = 0; i < count; i++) {
            uint32_t color =
            ((spans[i].coverage/2) << baton->rshift) |
            ((spans[i].coverage/2) << baton->gshift) |
            ((spans[i].coverage/2) << baton->bshift);
            
            uint32_t *start = scanline + spans[i].x;
            if (unlikely start + spans[i].len > baton->last_pixel)
                return;
            
            for (int x = 0; x < spans[i].len; x++)
                *start++ = color;
        }
    }
    
    /* This spanner does read/modify/write, trading performance for accuracy.
     The color here is simply half coverage value in all channels,
     effectively mid-gray.
     Suitable for when artifacts mostly do come up and annoy.
     This might be optimized if one does rmw only for some values of x.
     But since the whole buffer has to be rw anyway, and the previous value
     is probably still in the cache, there's little point to. */
    void spanner_rw(int y, int count, const FT_Span* spans, void *user) {
        spanner_baton_t *baton = (spanner_baton_t *) user;
        uint32_t *scanline = baton->pixels - y * ( (int) baton->pitch / 4 );
        if (unlikely scanline < baton->first_pixel)
            return;
        
        for (int i = 0; i < count; i++) {
            uint32_t color =
            ((spans[i].coverage/2)  << baton->rshift) |
            ((spans[i].coverage/2) << baton->gshift) |
            ((spans[i].coverage/2) << baton->bshift);
            uint32_t *start = scanline + spans[i].x;
            if (unlikely start + spans[i].len > baton->last_pixel)
                return;
            
            for (int x = 0; x < spans[i].len; x++)
                *start++ |= color;
        }
    }
    
    /*  This spanner is for obtaining exact bounding box for the string.
     Unfortunately this can't be done without rendering it (or pretending to).
     After this runs, we get min and max values of coordinates used.
     */
    void spanner_sizer(int y, int count, const FT_Span* spans, void *user) {
        spanner_baton_t *baton = (spanner_baton_t *) user;
        
        if (y < baton->min_y)
            baton->min_y = y;
        if (y > baton->max_y)
            baton->max_y = y;
        for (int i = 0 ; i < count; i++) {
            if (spans[i].x + spans[i].len > baton->max_span_x)
                baton->max_span_x = spans[i].x + spans[i].len;
            if (spans[i].x < baton->min_span_x)
                baton->min_span_x = spans[i].x;
        }
    }
    
    FT_SpanFunc spanner = spanner_wo;
    
    void ftfdump(FT_Face ftf) {
        for(int i=0; i<ftf->num_charmaps; i++) {
            printf("%d: %s %s %c%c%c%c plat=%hu id=%hu\n", i,
                   ftf->family_name,
                   ftf->style_name,
                   ftf->charmaps[i]->encoding >>24,
                   (ftf->charmaps[i]->encoding >>16 ) & 0xff,
                   (ftf->charmaps[i]->encoding >>8) & 0xff,
                   (ftf->charmaps[i]->encoding) & 0xff,
                   ftf->charmaps[i]->platform_id,
                   ftf->charmaps[i]->encoding_id
                   );
        }
    }
    
    /*  See http://www.microsoft.com/typography/otspec/name.htm
     for a list of some possible platform-encoding pairs.
     We're interested in 0-3 aka 3-1 - UCS-2.
     Otherwise, fail. If a font has some unicode map, but lacks
     UCS-2 - it is a broken or irrelevant font. What exactly
     Freetype will select on face load (it promises most wide
     unicode, and if that will be slower that UCS-2 - left as
     an excercise to check. */
    int force_ucs2_charmap(FT_Face ftf) {
        for(int i = 0; i < ftf->num_charmaps; i++)
            if ((  (ftf->charmaps[i]->platform_id == 0)
                 && (ftf->charmaps[i]->encoding_id == 3))
                || ((ftf->charmaps[i]->platform_id == 3)
                    && (ftf->charmaps[i]->encoding_id == 1)))
                return FT_Set_Charmap(ftf, ftf->charmaps[i]);
        return -1;
    }
    
    void hline(char* pixels, int pitch, int min_x, int max_x, int y, uint32_t color) {
        uint32_t *pix = (uint32_t *)pixels + (y * pitch) / 4 + min_x;
        uint32_t *end = (uint32_t *)pixels + (y * pitch) / 4 + max_x;
        
        while (pix - 1 != end)
            *pix++ = color;
    }
    
    void vline(char* pixels, int pitch, int min_y, int max_y, int x, uint32_t color) {
        uint32_t *pix = (uint32_t *)pixels + (min_y * pitch) / 4 + x;
        uint32_t *end = (uint32_t *)pixels + (max_y * pitch) / 4 + x;
        
        while (pix - pitch/4 != end) {
            *pix = color;
            pix += pitch/4;
        }
    }
    
    hb_font_t* createFontFromFile(std::string filename, int index)
    {
        hb_font_t* font = NULL;
        
        FILE* f = fopen((char*)filename.c_str(), "rb");
        
        if (f)
        {
            fseek(f, 0, SEEK_END);
            long size = ftell(f);
            rewind(f);
            
            // Memory is released in hb_blob_destroy.
            char* buffer = (char*)malloc(size);
            size_t readcount = fread(buffer, 1, size, f);
            fclose(f);
            
            hb_blob_t* blob = hb_blob_create((const char*)buffer, readcount, HB_MEMORY_MODE_WRITABLE, buffer, (hb_destroy_func_t)free);
            
            hb_face_t* face = hb_face_create(blob, index);
            font = hb_font_create(face);
            hb_ot_font_set_funcs(font);
            
            hb_blob_destroy(blob);
            hb_face_destroy(face);
        }
        
        return font;
    }
    
    
    hb_font_t* getHBFont(std::string name, FT_Face ft_face)
    {
        hb_font_t* font = createFontFromFile(name, 0);
        if (font)
        {
            int nXScale = (int) (((uint64_t) ft_face->size->metrics.x_scale * (uint64_t) ft_face->units_per_EM + (1<<15)) >> 16);
            int nYScale = (int) (((uint64_t) ft_face->size->metrics.y_scale * (uint64_t) ft_face->units_per_EM + (1<<15)) >> 16);
            hb_font_set_scale(font, nXScale, nYScale);
        }
        
        return font;
    }
    
    
    // Chunk of text with the same script.
    struct TextChunk
    {
        const wchar_t* text;
        size_t length;
        hb_script_t script;
    };
    
    
    // Use FriBiDI to sort text in right bi direction sequence
    FriBidiStrIndex* getReorderingMap(const wchar_t* text)
    {
        int nLineSize = wcslen(text);
        
        uint* pTempLogicalLine = new uint[nLineSize];
        uint* pTempVisualLine  = new uint[nLineSize];
        FriBidiStrIndex* pTempPositionLogicToVisual = new FriBidiStrIndex[nLineSize];
        FriBidiCharType* pTempBidiTypes    = new FriBidiCharType[nLineSize];
        FriBidiLevel* pTempEmbeddingLevels = new FriBidiLevel[nLineSize];
        FriBidiJoiningType* pTempJtypes    = new FriBidiJoiningType[nLineSize];
        FriBidiArabicProp*  pTempArProps   = new FriBidiArabicProp[nLineSize];
        
        for (int i = 0; i < nLineSize; ++i)
        {
            pTempLogicalLine[i] = text[i];
        }
        
        // Get letter types.
        fribidi_get_bidi_types(pTempLogicalLine, nLineSize, pTempBidiTypes);
        
        FriBidiParType baseDirection = FRIBIDI_PAR_LTR;
        FriBidiLevel   resolveParDir = fribidi_get_par_embedding_levels(pTempBidiTypes, nLineSize, &baseDirection, pTempEmbeddingLevels);

        // joine types.
        fribidi_get_joining_types(pTempLogicalLine, nLineSize, pTempJtypes);
        
        // arabic join.
        memcpy(pTempArProps, pTempJtypes, nLineSize * sizeof(FriBidiJoiningType));
        fribidi_join_arabic(pTempBidiTypes, nLineSize, pTempEmbeddingLevels, pTempArProps);
        
        // shapes.
        fribidi_shape (FRIBIDI_FLAG_SHAPE_MIRRORING | FRIBIDI_FLAG_SHAPE_ARAB_PRES | FRIBIDI_FLAG_SHAPE_ARAB_LIGA,
                       pTempEmbeddingLevels, nLineSize, pTempArProps, pTempLogicalLine);
        
        memcpy(pTempVisualLine, pTempLogicalLine, nLineSize * sizeof(uint));
        for (int i = 0; i < nLineSize; i ++)
        {
            pTempPositionLogicToVisual[i] = i;
        }
        
        FriBidiLevel levels = fribidi_reorder_line(FRIBIDI_FLAGS_ARABIC, pTempBidiTypes, nLineSize,
                                                0, baseDirection,  pTempEmbeddingLevels, pTempVisualLine, pTempPositionLogicToVisual);
        
        if (pTempJtypes) { delete[] pTempJtypes;}
        if (pTempArProps) { delete[] pTempArProps;}
        if (pTempLogicalLine) { delete[] pTempLogicalLine;}
        if (pTempEmbeddingLevels) { delete[] pTempEmbeddingLevels;}
        if (pTempBidiTypes) { delete[] pTempBidiTypes;}
        if (pTempVisualLine) { delete[] pTempVisualLine;}
        
        return pTempPositionLogicToVisual;
    }

    // Split string to chunks.
    std::vector<TextChunk> splitToChunks(const wchar_t* visibleText)
    {
        std::vector<TextChunk> res;
        
        size_t length = wcslen(visibleText);

        hb_unicode_funcs_t* ufuncs = hb_unicode_funcs_get_default();
        
        hb_script_t currentScript = hb_unicode_script(ufuncs, visibleText[0]);
        const wchar_t* chunkStart = visibleText;

        for (size_t i = 1; i < length; i++)
        {
            const wchar_t* currentText = &visibleText[i];
            hb_script_t script = hb_unicode_script(ufuncs, *currentText);
            
            // Skip for HB_SCRIPT_INHERITED, because it can be diacritics.
            if ((script != currentScript && script != HB_SCRIPT_INHERITED))
            {
                TextChunk chunk;
                chunk.text   = chunkStart;
                chunk.length = currentText - chunkStart;
                chunk.script = currentScript;
                res.push_back(chunk);
                
                chunkStart = currentText;
                currentScript = script;
            }
        }
        
        const wchar_t* lastSymbol = &visibleText[length - 1];
        if (chunkStart <= lastSymbol)
        {
            TextChunk chunk;
            chunk.text   = chunkStart;
            chunk.length = lastSymbol - chunkStart + 1;
            chunk.script = currentScript;
            res.push_back(chunk);
        }
        
        return res;
    }    

    int main () {
        int ptSize = 50*64;
        int device_hdpi = 72;
        int device_vdpi = 72;
        
        /* Init freetype */
        FT_Library ft_library;
        assert(!FT_Init_FreeType(&ft_library));
        
        
        std::string cppFile(__FULLPATH_FILE__);
#ifdef _WIN32
        std::string sourceFolder = cppFile.substr(0, cppFile.rfind("\\"));
#else
		std::string sourceFolder = cppFile.substr(0, cppFile.rfind("/"));
#endif
        std::string fontFilename = sourceFolder + "/fonts/arial.ttf";
        
        /* Load our fonts */
        FT_Face ft_face;
        assert(!FT_New_Face(ft_library, fontFilename.c_str(), 0, &ft_face));
        assert(!FT_Set_Char_Size(ft_face, 0, ptSize, device_hdpi, device_vdpi ));
        ftfdump(ft_face); // wonderful world of encodings ...
        force_ucs2_charmap(ft_face); // which we ignore.
        
        /* Get our harfbuzz font structs */
        hb_font_t *hb_ft_font = getHBFont(fontFilename.c_str(), ft_face);
        
        /** Setup our SDL window **/
        int width      = 1000;
        int height     = 300;
        
        /* Initialize our SDL window */
        if(SDL_Init(SDL_INIT_VIDEO) < 0)
        {
            fprintf(stderr, "Failed to initialize SDL");
            return -1;
        }
        
        SDL_Window *screen = SDL_CreateWindow("SDL2 + FreeType + HarfBuzz + FriBiDi Example",
                                              SDL_WINDOWPOS_UNDEFINED,
                                              SDL_WINDOWPOS_UNDEFINED,
                                              width, height,
                                              SDL_WINDOW_RESIZABLE | SDL_WINDOW_OPENGL);
        
        /* Create an SDL texture we can draw to */
        SDL_Renderer *renderer   = SDL_CreateRenderer(screen, -1, SDL_RENDERER_ACCELERATED);
        SDL_Texture  *sdlTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, width, height);
        
        /* Create a buffer for harfbuzz to use */
        hb_buffer_t *buf = hb_buffer_create();
        
        /* Run fribidi to get correct postion for each letter */
        FriBidiStrIndex* reorderPosition = getReorderingMap(text);
        const wchar_t* visibleText = text;
        // Split text string to text chunks.
        std::vector<TextChunk> textChunks = splitToChunks(visibleText);
        
        /* Our main event/draw loop */
        int done = 0;
        int resized = 1;
        while (!done)
        {
            SDL_Rect rect = {0, 0, width, height};
            
            char*          pixels = NULL;
            int            pitch = 0;
            
            /* Clear our texture */
            SDL_LockTexture(sdlTexture,
                            &rect,
                            (void**)&pixels,
                            &pitch);
            
            memset(pixels, 0, pitch * height);
            
            /* The pen/baseline start coordinates in window coordinate system
             - with those text placement in the window is controlled.
             - note that for RTL scripts pen still goes LTR */
            int x = 0, y = 50 + 75;
            x = 20;
            
            for (std::vector<TextChunk>::iterator chunk = textChunks.begin();
                 chunk != textChunks.end(); chunk++)
            {
                hb_direction_t dir = hb_script_get_horizontal_direction(chunk->script);
                hb_buffer_set_direction(buf, dir); /* or LTR */
                hb_buffer_set_script(buf, chunk->script); /* see hb-unicode.h */
                //hb_buffer_set_language(buf, hb_language_from_string(language, strlen(language)));
                
                /* Layout the text */
#ifdef _WIN32
				hb_buffer_add_utf16(buf, (const uint16_t*)chunk->text, chunk->length, 0, chunk->length);
#else
				hb_buffer_add_utf32(buf, (const uint32_t*)chunk->text, chunk->length, 0, chunk->length);
#endif
                hb_shape(hb_ft_font, buf, NULL, 0);

                
                unsigned int         glyph_count;
                hb_glyph_info_t     *glyph_info   = hb_buffer_get_glyph_infos(buf, &glyph_count);
                hb_glyph_position_t *glyph_pos    = hb_buffer_get_glyph_positions(buf, &glyph_count);
                
                /* set up rendering via spanners */
                spanner_baton_t stuffbaton;
                
                FT_Raster_Params ftr_params;
                ftr_params.target = 0;
                ftr_params.flags = FT_RASTER_FLAG_DIRECT | FT_RASTER_FLAG_AA;
                ftr_params.user = &stuffbaton;
                ftr_params.black_spans = 0;
                ftr_params.bit_set = 0;
                ftr_params.bit_test = 0;
                
                /* Calculate string bounding box in pixels */
                ftr_params.gray_spans = spanner_sizer;
                
                /* See http://www.freetype.org/freetype2/docs/glyphs/glyphs-3.html */
                
                int max_x = INT_MIN; // largest coordinate a pixel has been set at, or the pen was advanced to.
                int min_x = INT_MAX; // smallest coordinate a pixel has been set at, or the pen was advanced to.
                int max_y = INT_MIN; // this is max topside bearing along the string.
                int min_y = INT_MAX; // this is max value of (height - topbearing) along the string.
                /*  Naturally, the above comments swap their meaning between horizontal and vertical scripts,
                 since the pen changes the axis it is advanced along.
                 However, their differences still make up the bounding box for the string.
                 Also note that all this is in FT coordinate system where y axis points upwards.
                 */
                
                int sizer_x = 0;
                int sizer_y = 0; /* in FT coordinate system. */
                
                FT_Error fterr;
                //unsigned start = (dir == HB_DIRECTION_LTR) ? 0 : glyph_count - 1;
                for (unsigned j = 0; j < glyph_count; ++j) {
                //for (unsigned j = 0; j < glyph_count; ++j) {
                    if ((fterr = FT_Load_Glyph(ft_face, glyph_info[j].codepoint, 0))) {
                        printf("load %08x failed fterr=%d.\n",  glyph_info[j].codepoint, fterr);
                    } else {
                        if (ft_face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
                            printf("glyph->format = %4s\n", (char *)&ft_face->glyph->format);
                        } else {
                            int gx = sizer_x + (glyph_pos[j].x_offset/64);
                            int gy = sizer_y + (glyph_pos[j].y_offset/64); // note how the sign differs from the rendering pass
                            
                            stuffbaton.min_span_x = INT_MAX;
                            stuffbaton.max_span_x = INT_MIN;
                            stuffbaton.min_y = INT_MAX;
                            stuffbaton.max_y = INT_MIN;
                            
                            if ((fterr = FT_Outline_Render(ft_library, &ft_face->glyph->outline, &ftr_params)))
                                printf("FT_Outline_Render() failed err=%d\n", fterr);
                            
                            if (stuffbaton.min_span_x != INT_MAX) {
                                /* Update values if the spanner was actually called. */
                                if (min_x > stuffbaton.min_span_x + gx)
                                    min_x = stuffbaton.min_span_x + gx;
                                
                                if (max_x < stuffbaton.max_span_x + gx)
                                    max_x = stuffbaton.max_span_x + gx;
                                
                                if (min_y > stuffbaton.min_y + gy)
                                    min_y = stuffbaton.min_y + gy;
                                
                                if (max_y < stuffbaton.max_y + gy)
                                    max_y = stuffbaton.max_y + gy;
                            } else {
                                /* The spanner wasn't called at all - an empty glyph, like space. */
                                if (min_x > gx) min_x = gx;
                                if (max_x < gx) max_x = gx;
                                if (min_y > gy) min_y = gy;
                                if (max_y < gy) max_y = gy;
                            }
                        }
                    }
                    
                    sizer_x += glyph_pos[j].x_advance/64;
                    sizer_y += glyph_pos[j].y_advance/64; // note how the sign differs from the rendering pass
                    
                }
                
                /* Still have to take into account last glyph's advance. Or not? */
                if (min_x > sizer_x) min_x = sizer_x;
                if (max_x < sizer_x) max_x = sizer_x;
                if (min_y > sizer_y) min_y = sizer_y;
                if (max_y < sizer_y) max_y = sizer_y;
                
                /* The bounding box */
                int bbox_w = max_x - min_x;
                int bbox_h = max_y - min_y;
                
                /* Two offsets below position the bounding box with respect to the 'origin',
                 which is sort of origin of string's first glyph.
                 
                 baseline_offset - offset perpendecular to the baseline to the topmost (horizontal),
                 or leftmost (vertical) pixel drawn.
                 
                 baseline_shift  - offset along the baseline, from the first drawn glyph's origin
                 to the leftmost (horizontal), or topmost (vertical) pixel drawn.
                 
                 Thus those offsets allow positioning the bounding box to fit the rendered string,
                 as they are in fact offsets from the point given to the renderer, to the top left
                 corner of the bounding box.
                 
                 NB: baseline is defined as y==0 for horizontal and x==0 for vertical scripts.
                 (0,0) here is where the first glyph's origin ended up after shaping, not taking
                 into account glyph_pos[0].xy_offset (yeah, my head hurts too).
                 */
                
                int baseline_offset;
                int baseline_shift;
                
                if (HB_DIRECTION_IS_HORIZONTAL(hb_buffer_get_direction(buf))) {
                    baseline_offset = max_y;
                    baseline_shift  = min_x;
                }
                if (HB_DIRECTION_IS_VERTICAL(hb_buffer_get_direction(buf))) {
                    baseline_offset = min_x;
                    baseline_shift  = max_y;
                }
                
                if (resized)
                    printf("string min_x=%d max_x=%d min_y=%d max_y=%d bbox %dx%d boffs %d,%d\n",
                           min_x, max_x, min_y, max_y, bbox_w, bbox_h, baseline_offset, baseline_shift);
                /*
                 /* Draw baseline and the bounding box */
                /* The below is complicated since we simultaneously
                 convert to the window coordinate system. */
                int left, right, top, bottom;
                
                if (HB_DIRECTION_IS_HORIZONTAL(hb_buffer_get_direction(buf))) {
                    /* bounding box in window coordinates without offsets */
                    left   = x;
                    right  = x + bbox_w;
                    top    = y - bbox_h;
                    bottom = y;
                    
                    /* apply offsets */
                    left   +=  baseline_shift;
                    right  +=  baseline_shift;
                    top    -=  baseline_offset - bbox_h;
                    bottom -=  baseline_offset - bbox_h;
                    
                    /* draw the baseline */
                    hline(pixels, pitch, x, x + bbox_w, y, 0x0000ff00);
                }
                
                if (HB_DIRECTION_IS_VERTICAL(hb_buffer_get_direction(buf))) {
                    left   = x;
                    right  = x + bbox_w;
                    top    = y;
                    bottom = y + bbox_h;
                    
                    left   += baseline_offset;
                    right  += baseline_offset;
                    top    -= baseline_shift;
                    bottom -= baseline_shift;
                    
                    vline(pixels, pitch, y, y + bbox_h, x, 0x0000ff00);
                }
                if (resized)
                    printf("origin %d,%d bbox l=%d r=%d t=%d b=%d\n",
                           x, y, left, right, top, bottom);
                
                /* +1/-1 are for the bbox borders be the next pixel outside the bbox itself */
                hline(pixels, pitch, left - 1, right + 1, top - 1, 0x00ff0000);
                hline(pixels, pitch, left - 1, right + 1, bottom + 1, 0x00ff0000);
                vline(pixels, pitch, top - 1, bottom + 1, left - 1, 0x00ff0000);
                vline(pixels, pitch, top - 1, bottom + 1, right + 1, 0x00ff0000);
                
                /* set rendering spanner */
                ftr_params.gray_spans = spanner;
                
                /* initialize rendering part of the baton */
                stuffbaton.pixels = NULL;
                stuffbaton.first_pixel = (uint32_t*)pixels;
                stuffbaton.last_pixel = (uint32_t *) (((uint8_t *) pixels) + pitch * height);
                stuffbaton.pitch = pitch;
                stuffbaton.rshift = 0;
                stuffbaton.gshift = 8;
                stuffbaton.bshift = 16;
                
                /* render */
                for (unsigned j = 0; j < glyph_count; ++j)
                {
                    // We use reordering indexes to set letters to correct position for bidirectional text.
                    int visiblePosition = reorderPosition[j];
                    if ((fterr = FT_Load_Glyph(ft_face, glyph_info[visiblePosition].codepoint, 0))) {
                        printf("load %08x failed fterr=%d.\n",  glyph_info[visiblePosition].codepoint, fterr);
                    } else {
                        if (ft_face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) {
                            printf("glyph->format = %4s\n", (char *)&ft_face->glyph->format);
                        } else {
                            int gx = x + (glyph_pos[visiblePosition].x_offset/64);
                            int gy = y - (glyph_pos[visiblePosition].y_offset/64);
                            
                            stuffbaton.pixels = (uint32_t *)(((uint8_t *) pixels) + gy * pitch) + gx;
                            
                            if ((fterr = FT_Outline_Render(ft_library, &ft_face->glyph->outline, &ftr_params)))
                                printf("FT_Outline_Render() failed err=%d\n", fterr);
                        }
                    }
                    
                    x += glyph_pos[visiblePosition].x_advance/64;
                    y -= glyph_pos[visiblePosition].y_advance/64;
                }
                
                hb_buffer_clear_contents(buf);
            }
            
            /* clean up the buffer, but don't kill it just yet */
            hb_buffer_clear_contents(buf);
            
            //}
            
            SDL_UnlockTexture(sdlTexture);
            
            /* Blit our new image to our visible screen */
            
            SDL_RenderClear(renderer);
            SDL_RenderCopy(renderer, sdlTexture, NULL, NULL);
            SDL_RenderPresent(renderer);
            
            /* Handle SDL events */
            SDL_Event event;
            resized = resized ? !resized : resized;
            while(SDL_PollEvent(&event))
            {
                switch (event.type) {
                    case SDL_KEYDOWN:
                        if (event.key.keysym.sym == SDLK_ESCAPE) {
                            done = 1;
                        }
                        break;
                    case SDL_QUIT:
                        done = 1;
                        break;
                }
            }
            
            SDL_Delay(150);
        }
        
        
        /* Cleanup */
        hb_buffer_destroy(buf);
        hb_font_destroy(hb_ft_font);
        
        FT_Done_FreeType(ft_library);
        
        SDL_DestroyTexture(sdlTexture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(screen);
        
        SDL_Quit();
        
        return 0;
    }
    
}

int main ()
{
    return Example::main();
}
