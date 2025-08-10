#include "xml_parse.h"
#include <expat.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct {
    XML_Parser parser;
    int in_item;
    int in_title;
    int in_description;
    char title[256];
    char *description;     // Now dynamically allocated
    size_t description_size;
    size_t description_capacity;
    char *found_url;  // Store the found URL (for single URL function)
    char **all_urls;  // Store all found URLs
    int url_count;    // Number of URLs found
    int url_capacity; // Capacity of the all_urls array
} ctx_t;

static void start_elem(void *data, const char *el, const char **attr) {
    ctx_t *c = data;
    if (strcmp(el, "item") == 0) {
        c->in_item = 1;
        c->title[0] = '\0';
        
        // Initialize description buffer if not already allocated
        if (!c->description) {
            c->description_capacity = 4096;
            c->description = malloc(c->description_capacity);
            if (!c->description) {
                fprintf(stderr, "Failed to allocate description buffer\n");
                return;
            }
        }
        c->description[0] = '\0';
        c->description_size = 0;
    } else if (c->in_item && strcmp(el, "title") == 0) {
        c->in_title = 1;
    } else if (c->in_item && strcmp(el, "description") == 0) {
        c->in_description = 1;
    }
}

static void add_url_to_array(ctx_t *c, const char *url) {
    // Expand array if needed
    if (c->url_count >= c->url_capacity) {
        int new_capacity = c->url_capacity == 0 ? 4 : c->url_capacity * 2;
        char **new_array = realloc(c->all_urls, new_capacity * sizeof(char *));
        if (!new_array) {
            fprintf(stderr, "Memory allocation failed for URL array\n");
            return;
        }
        c->all_urls = new_array;
        c->url_capacity = new_capacity;
    }
    
    // Add the URL
    c->all_urls[c->url_count] = strdup(url);
    if (c->all_urls[c->url_count]) {
        c->url_count++;
    } else {
        fprintf(stderr, "Memory allocation failed for URL string\n");
    }
}

static void end_elem(void *data, const char *el) {
    ctx_t *c = data;
    if (strcmp(el, "item") == 0) {
        if (strstr(c->title, "Graphics")) {
            char *p = strstr(c->description, "src=\"");
            if (p) {
                p += 5;
                char *q = strchr(p, '"');
                if (q) {
                    *q = '\0';
                    printf("Storm Graphics URL: %s\n", p);
                    
                    // For single URL function (backward compatibility)
                    if (!c->found_url) {
                        c->found_url = strdup(p);
                    }
                    
                    // For multiple URLs function
                    if (c->all_urls != NULL || c->url_capacity > 0) {
                        add_url_to_array(c, p);
                    }
                    
                    *q = '"';  // Restore the quote
                }
            } else {
                p = strstr(c->description, "href=\"");
                if (p) {
                    p += 6;
                    char *q = strchr(p, '"');
                    if (q) {
                        *q = '\0';
                        printf("Storm Cone Page URL: %s\n", p);
                        *q = '"';  // Restore the quote
                    }
                }
            }
        }
        c->in_item = 0;
    } else if (strcmp(el, "title") == 0) {
        c->in_title = 0;
    } else if (strcmp(el, "description") == 0) {
        c->in_description = 0;
    }
}

static void char_data(void *data, const char *s, int len) {
    ctx_t *c = data;
    if (c->in_title) {
        // Safely append to title with bounds checking
        size_t current_len = strlen(c->title);
        size_t available = sizeof(c->title) - current_len - 1;
        if (available > 0) {
            size_t copy_len = (len < available) ? len : available;
            strncat(c->title, s, copy_len);
        }
    } else if (c->in_description && c->description) {
        // Safely append to description with dynamic reallocation if needed
        size_t needed = c->description_size + len + 1;  // +1 for null terminator
        
        if (needed > c->description_capacity) {
            // Expand description buffer
            size_t new_capacity = c->description_capacity == 0 ? 4096 : c->description_capacity * 2;
            while (new_capacity < needed) {
                new_capacity *= 2;
            }
            
            char *new_desc = realloc(c->description, new_capacity);
            if (new_desc) {
                c->description = new_desc;
                c->description_capacity = new_capacity;
            } else {
                // Memory allocation failed, skip this data
                return;
            }
        }
        
        // Append the new data
        memcpy(c->description + c->description_size, s, len);
        c->description_size += len;
        c->description[c->description_size] = '\0';  // Null terminate
    }
}

void parse_feed_buffer(const char *buf, size_t len) {
    ctx_t ctx = {0};
    ctx.parser = XML_ParserCreate(NULL);
    XML_SetUserData(ctx.parser, &ctx);
    XML_SetElementHandler(ctx.parser, start_elem, end_elem);
    XML_SetCharacterDataHandler(ctx.parser, char_data);

    // Feed the whole buffer at once
    if (!XML_Parse(ctx.parser, buf, (int)len, 1)) {
        fprintf(stderr, "Parse error: %s\n",
                XML_ErrorString(XML_GetErrorCode(ctx.parser)));
        // Optionally handle parse error here
    }
    XML_ParserFree(ctx.parser);
    
    // Clean up description buffer
    if (ctx.description) {
        free(ctx.description);
    }
}

void parse_feed(const char *buf, size_t len) {
    parse_feed_buffer(buf, len);
}

char *xml_parse_cone_image_url(const char *buf, size_t len) {
    ctx_t ctx = {0};
    ctx.parser = XML_ParserCreate(NULL);
    XML_SetUserData(ctx.parser, &ctx);
    XML_SetElementHandler(ctx.parser, start_elem, end_elem);
    XML_SetCharacterDataHandler(ctx.parser, char_data);

    // Feed the whole buffer at once
    if (!XML_Parse(ctx.parser, buf, (int)len, 1)) {
        fprintf(stderr, "Parse error: %s\n",
                XML_ErrorString(XML_GetErrorCode(ctx.parser)));
        // Optionally handle parse error here
    }
    XML_ParserFree(ctx.parser);
    
    // Clean up description buffer
    if (ctx.description) {
        free(ctx.description);
    }
    
    return ctx.found_url;  // Caller must free this
}

char **xml_parse_all_cone_image_urls(const char *buf, size_t len, int *count) {
    ctx_t ctx = {0};
    
    // Initialize for collecting multiple URLs
    ctx.url_capacity = 4;  // Start with capacity for 4 URLs
    ctx.all_urls = malloc(ctx.url_capacity * sizeof(char *));
    if (!ctx.all_urls) {
        *count = 0;
        return NULL;
    }
    
    ctx.parser = XML_ParserCreate(NULL);
    XML_SetUserData(ctx.parser, &ctx);
    XML_SetElementHandler(ctx.parser, start_elem, end_elem);
    XML_SetCharacterDataHandler(ctx.parser, char_data);

    // Feed the whole buffer at once
    if (!XML_Parse(ctx.parser, buf, (int)len, 1)) {
        fprintf(stderr, "Parse error: %s\n",
                XML_ErrorString(XML_GetErrorCode(ctx.parser)));
        // Clean up on parse error
        xml_parse_free_urls(ctx.all_urls, ctx.url_count);
        if (ctx.description) {
            free(ctx.description);
        }
        XML_ParserFree(ctx.parser);
        *count = 0;
        return NULL;
    }
    XML_ParserFree(ctx.parser);
    
    // Clean up description buffer
    if (ctx.description) {
        free(ctx.description);
    }
    
    *count = ctx.url_count;
    
    // If no URLs found, clean up and return NULL
    if (ctx.url_count == 0) {
        free(ctx.all_urls);
        return NULL;
    }
    
    // Shrink array to actual size to save memory
    if (ctx.url_count < ctx.url_capacity) {
        char **resized = realloc(ctx.all_urls, ctx.url_count * sizeof(char *));
        if (resized) {
            ctx.all_urls = resized;
        }
    }
    
    return ctx.all_urls;  // Caller must free with xml_parse_free_urls
}

void xml_parse_free_urls(char **urls, int count) {
    if (!urls) return;
    
    for (int i = 0; i < count; i++) {
        free(urls[i]);
    }
    free(urls);
}
