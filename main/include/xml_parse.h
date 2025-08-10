#ifndef XML_PARSE_H
#define XML_PARSE_H

#include <stdio.h>

/**
 * @brief Parses the National Hurricane Center XML feed and prints storm graphics URLs.
 *
 * This function takes an open file stream (e.g. from esp_http_client to a temporary file,
 * or a static file in SPIFFS/SD) and parses the XML using Expat. It looks for items
 * with a <title> containing "Graphics" and extracts the URL of the cone graphic image.
 *
 * Example use:
 *     FILE *f = fopen("/spiffs/index-at.xml", "r");
 *     if (f) {
 *         parse_feed(f);
 *         fclose(f);
 *     }
 *
 * @param f FILE pointer to the XML feed.
 */
void parse_feed(const char *buf, size_t len);

/**
 * @brief Parses the National Hurricane Center XML feed and extracts cone image URL.
 *
 * This function parses the XML buffer looking for items with "Graphics" in the title
 * and extracts the cone image URL from the description.
 *
 * @param buf Buffer containing XML data.
 * @param len Length of the buffer.
 * @return Allocated string containing the cone image URL, or NULL if not found.
 *         Caller must free the returned string.
 */
char *xml_parse_cone_image_url(const char *buf, size_t len);

/**
 * @brief Parses the National Hurricane Center XML feed and extracts all cone image URLs.
 *
 * This function parses the XML buffer looking for items with "Graphics" in the title
 * and extracts all cone image URLs from their descriptions.
 *
 * @param buf Buffer containing XML data.
 * @param len Length of the buffer.
 * @param count Pointer to store the number of URLs found.
 * @return Array of allocated strings containing cone image URLs, or NULL if none found.
 *         Caller must free each string in the array and the array itself.
 */
char **xml_parse_all_cone_image_urls(const char *buf, size_t len, int *count);

/**
 * @brief Frees the array of URLs returned by xml_parse_all_cone_image_urls.
 *
 * @param urls Array of URL strings.
 * @param count Number of URLs in the array.
 */
void xml_parse_free_urls(char **urls, int count);

#endif // XML_PARSE_H
