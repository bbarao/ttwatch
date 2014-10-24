/******************************************************************************\
** ttwatch.c                                                                  **
** Main implementation file for the TomTom watch linux driver                 **
\******************************************************************************/

#include "messages.h"
#include "ttbin.h"
#include "log.h"

#include <getopt.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <unistd.h>
#include <pwd.h>
#include <grp.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <libusb-1.0/libusb.h>

#include <curl/curl.h>

/*************************************************************************************************/

#define TOMTOM_VENDOR_ID    (0x1390)
#define TOMTOM_PRODUCT_ID   (0x7474)

/*************************************************************************************************/

typedef struct
{
    uint8_t *data;
    uint32_t length;
} DOWNLOAD;

typedef struct
{
    uint32_t id;
    DOWNLOAD download;
} FIRMWARE_FILE;

typedef struct
{
    int update_firmware;
    int update_gps;
#ifdef UNSAFE
    int list_files;
    int read_file;
    int write_file;
    uint32_t file_id;
#endif
    int select_device;
    int device_number;
    int show_versions;
    int list_devices;
    int get_time;
    int set_time;
    int get_activities;
    int get_name;
    int set_name;
    char *watch_name;
    int list_formats;
    int set_formats;
    char *formats;
    int daemon_mode;
    int run_as;
    char *run_as_user;
#ifdef UNSAFE
    char *file;
#endif
    char *activity_store;
} OPTIONS;


/*************************************************************************************************/

int show_packets = 0;

/*************************************************************************************************/

libusb_device_handle *open_usb_device(int list_devices, int select_device, int device_number)
{
    libusb_device **list = 0;
    libusb_device_handle *selected_handle = 0;
    ssize_t count = 0;
    ssize_t i;
    unsigned index = 0;
    unsigned char serial[64];

    count = libusb_get_device_list(NULL, &list);
    for (i = 0; i < count; ++i)
    {
        struct libusb_device_descriptor desc;
        int result;
        libusb_device_handle *handle;

        /* get the device descriptor */
        libusb_get_device_descriptor(list[i], &desc);

        /* ignore any non-TomTom devices */
        /* PID 0x7474 is Multisport and Multisport Cardio */
        if ((desc.idVendor  != TOMTOM_VENDOR_ID) ||
            (desc.idProduct != TOMTOM_PRODUCT_ID))
        {
            continue;
        }

        /* open the device so we can read the serial number */
        if (!(result = libusb_open(list[i], &handle)))
        {
            int count = libusb_get_string_descriptor_ascii(handle, desc.iSerialNumber, serial, sizeof(serial));
            serial[count] = 0;

            /* print the device list if requested */
            if (list_devices)
                write_log(0, "Device %u: %s\n", index, serial);

            /* select this device if it's the one required */
            if (!select_device || (device_number == index))
                selected_handle = handle;

            ++index;

            /* close the handle again if this is not the chosen device */
            if (selected_handle != handle)
                libusb_close(handle);
        }
        else
            write_log(1, "Unable to open device: %d\n", result);
    }

    libusb_free_device_list(list, 1);
    return selected_handle;
}

void show_device_versions(libusb_device_handle *device)
{
    uint32_t product_id, ble_version;
    RXGetFirmwareVersionPacket response = {0};
    char buf[128];

    product_id = get_product_id(device);
    if (!product_id)
    {
        write_log(1, "Unable to read product ID\n");
        return;
    }
    write_log(0, "Product ID: 0x%08x\n", product_id);

    ble_version = get_ble_version(device);
    if (!ble_version)
    {
        write_log(1, "Unable to read BLE version\n");
        return;
    }
    write_log(0, "BLE Version: %u\n", ble_version);

    if (send_packet(device, MSG_GET_FIRMWARE_VERSION, 0, 0, 64, (uint8_t*)&response))
    {
        write_log(1, "Unable to read firmware version\n");
        return;
    }
    write_log(0, "Firmware Version: %s\n", response.version);

    if (get_watch_name(device, buf, sizeof(buf)))
    {
        write_log(1, "Unable to read watch name\n");
        return;
    }
    write_log(0, "Watch Name: %s\n", buf);

    if (get_serial_number(device, buf, sizeof(buf)))
    {
        write_log(1, "Unable to get watch serial number\n");
        return;
    }
    write_log(0, "Serial Number: %s\n", buf);
}

RXFindFilePacket *get_file_list(libusb_device_handle *device)
{
    RXFindFilePacket *files = (RXFindFilePacket *)malloc(100 * sizeof(RXFindFilePacket));
    RXFindFilePacket *file = files;
    if (find_first_file(device, file))
    {
        free(files);
        return NULL;
    }
    while (!file->end_of_list)
    {
        if (find_next_file(device, ++file))
            break;
    }
    send_message_group_1(device);

    return files;
}

#ifdef UNSAFE
void show_file_list(libusb_device_handle *device)
{
    RXFindFilePacket file;
    if (find_first_file(device, &file))
        return;
    while (!file.end_of_list)
    {
        write_log(0, "0x%08x: %u\n", file.id, file.file_size);
        if (find_next_file(device, &file))
            break;
    }
    send_message_group_1(device);
}

void do_read_file(libusb_device_handle *device, uint32_t id, FILE *file)
{
    uint32_t size = 0;
    uint8_t *data = 0;

    /* read the file data */
    data = read_whole_file(device, id, &size);
    if (size && data)
    {
        /* save the file data */
        fwrite(data, 1, size, file);
        free(data);
    }
}

void do_write_file(libusb_device_handle *device, uint32_t id, FILE *file)
{
    uint32_t size = 0;
    uint8_t *data = 0;

    /* read in the whole file */
    while (!feof(file))
    {
        data = (uint8_t*)realloc(data, size + 256);
        size += fread(data + size, 1, 256, file);
    }

    /* write the file to the device */
    write_verify_whole_file(device, id, data, size);

    free(data);
}

void read_all_files(libusb_device_handle *device)
{
    /* retrieve the file list */
    RXFindFilePacket *file;
    RXFindFilePacket *files = get_file_list(device);
    if (!files)
        return;

    /* loop through and save each file */
    file = files;
    while (!file->end_of_list)
    {
        char filename[16];
        FILE *f;
        sprintf(filename, "%08x.bin", file->id);
        if (f = fopen(filename, "w"))
        {
            do_read_file(device, file->id, f);
            fclose(f);
        }
        ++file;
    }

    free(files);
}
#endif  /* UNSAFE */

static size_t write_download(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    DOWNLOAD *download = (DOWNLOAD*)userdata;
    /* reallocate storage for the data to fit the extra size */
    size_t length = size * nmemb;
    download->data = realloc(download->data, download->length + length);

    /* store the new data and update the data length */
    memcpy(download->data + download->length, ptr, length);
    download->length += length;
    return length;
}

static int download_file(const char *url, DOWNLOAD *download)
{
    int result;
    CURL *curl = curl_easy_init();
    if (!curl)
    {
        write_log(1, "Unable to initialise libcurl\n");
        return 1;
    }

    download->data   = 0;
    download->length = 0;

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "curl/7.35.0");
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 50);
    curl_easy_setopt(curl, CURLOPT_TCP_KEEPALIVE, 1);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, download);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_download);

    result = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (result != CURLE_OK)
    {
        write_log(1, "Unable to download file: %s (%d)\n", url, result);
        return 1;
    }
    return 0;
}

void do_update_gps(libusb_device_handle *device)
{
    DOWNLOAD download = {0};
    uint32_t size = 0;
    uint8_t *data = 0;

    /* download the data file */
    write_log(0, "Downloading GPSQuickFix data file...\n");
    if (download_file("http://gpsquickfix.services.tomtom.com/fitness/sifgps.f2p3enc.ee", &download))
    {
        free(download.data);
        return;
    }

    write_log(0, "Writing file to watch...\n");
    switch (write_verify_whole_file(device, FILE_GPSQUICKFIX_DATA, download.data, download.length))
    {
    case 0:
        write_log(0, "GPSQuickFix data updated\n");

        /* not sure what this message does, but the official software
           sends it after updating this data, so we'll do it too */
        end_gps_update(device);
        break;
    case 3:
        write_log(1, "Verify failed\n");
    default:    /* intentional fallthrough */
        write_log(1, "GPSQuickFix data update failed\n");
        break;
    }

    free(download.data);
}

uint8_t *update_preferences_modified_time(uint8_t *data, uint32_t *size)
{
    static const char *const DAYNAMES[]   = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
    static const char *const MONTHNAMES[] = { "Jan", "Feb", "Mar", "May", "Apr", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
    char timestr[64];
    time_t rawtime;
    struct tm *timeinfo;
    uint8_t *start, *end;

    /* find the location of the modified time */
    start = strstr(data, "modified=\"");
    if (start)
    {
        start += 10;
        end = strchr(start, '\"');
    }

    if (start && end)
    {
        int len, diff;
        uint8_t *new_data;

        /* get the current time and format it as required */
        time(&rawtime);
        timeinfo = localtime(&rawtime);
        len = sprintf(timestr, "%s %d. %s %02d:%02d:%02d %04d",
             DAYNAMES[timeinfo->tm_wday], timeinfo->tm_mday,
             MONTHNAMES[timeinfo->tm_mon], timeinfo->tm_hour,
             timeinfo->tm_min, timeinfo->tm_sec, timeinfo->tm_year + 1900);

        /* find the difference in length between the two strings and
           adjust the size of the preferences file accordingly */
        diff = len - (end - start);
        new_data = malloc(*size + diff);

        memcpy(new_data, data, start - data);
        memcpy(new_data + (start - data), timestr, len);
        memcpy(new_data + (start - data) + len, end, data + *size - end + 1);

        *size += diff;

        free(data);
        return new_data;
    }

    return data;
}

void do_get_time(libusb_device_handle *device)
{
    char timestr[64];
    time_t time;
    if (get_watch_time(device, &time))
        return;

    strftime(timestr, sizeof(timestr), "%Y-%m-%d %H:%M:%S", gmtime(&time));

    write_log(0, "GPS time: %s (UTC)\n", timestr);
}

void do_set_time(libusb_device_handle *device)
{
    uint32_t size = 0;
    uint8_t *data = 0;

    /* read the preferences XML file */
    data = read_whole_file(device, FILE_PREFERENCES_XML, &size);
    if (size && data)
    {
        data = update_preferences_modified_time(data, &size);

        /* write back the new preferences XML file data */
        switch (write_verify_whole_file(device, FILE_PREFERENCES_XML, data, size))
        {
        case 0:
            write_log(0, "Time updated\n");
            break;
        case 3:
            write_log(1, "Verify error\n");
        default:
            write_log(1, "Time update failed\n");
            break;
        }

        free(data);
    }
}

/* performs a 'mkdir -p', i.e. creates an entire directory tree */
static void _mkdir(const char *dir)
{
    char tmp[256];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", dir);
    len = strlen(tmp);
    if(tmp[len - 1] == '/')
        tmp[len - 1] = 0;
    for(p = tmp + 1; *p; p++)
    {
        if(*p == '/')
        {
            *p = 0;
            mkdir(tmp, 0755);
            *p = '/';
        }
    }
    mkdir(tmp, 0755);
}

void do_get_activities(libusb_device_handle *device, const char *store, char **formats)
{
    char filename[256] = {0};
    char **ptr;

    /* read the list of all files so we can find the activity files */
    RXFindFilePacket *file;
    RXFindFilePacket *files = get_file_list(device);
    if (!files)
        return;

    file = files;
    for (file = files; !file->end_of_list; ++file)
    {
        uint32_t size;
        uint8_t *data;
        TTBIN_FILE *ttbin;
        FILE *f;
        struct tm *timestamp;

        /* check if this is a ttbin activity file */
        if ((file->id & FILE_TTBIN_MASK) != FILE_TTBIN_DATA)
            continue;

        data = read_whole_file(device, file->id, &size);
        if (!data)
            continue;

        /* parse the activity file */
        ttbin = parse_ttbin_data(data, size);
        if (formats)
        {
            write_log(0, "Downloading elevation data\n");
            download_elevation_data(ttbin);
        }

        timestamp = gmtime(&ttbin->timestamp);

        /* create the directory name: [store]/[watch name]/[date] */
        strcpy(filename, store);
        strcat(filename, "/");
        if (!get_watch_name(device, filename + strlen(filename), sizeof(filename) - strlen(filename)))
            strcat(filename, "/");
        sprintf(filename + strlen(filename), "%04d-%02d-%02d",
            timestamp->tm_year + 1900, timestamp->tm_mon + 1, timestamp->tm_mday);
        _mkdir(filename);
        chdir(filename);

        /* create the file name */
        sprintf(filename, "%s", create_filename(ttbin, "ttbin"));

        /* write the ttbin file */
        f = fopen(filename, "w");
        if (f)
        {
            fwrite(data, 1, size, f);
            fclose(f);

            /* delete the file from the watch */
            delete_file(device, file->id);
        }
        else
            write_log(1, "Unable to write file: %s\n", filename);

        if (formats)
        {
            ptr = formats;
            for(; *ptr; ++ptr)
            {
                sprintf(filename, "%s", create_filename(ttbin, *ptr));
                f = fopen(filename, "w");
                if (f)
                {
                    if (!strcasecmp(*ptr, "csv"))
                        export_csv(ttbin, f);
                    else if (!strcasecmp(*ptr, "kml"))
                        export_kml(ttbin, f);
                    else if (!strcasecmp(*ptr, "gpx"))
                        export_gpx(ttbin, f);

                    fclose(f);
                }
                else
                    write_log(1, "Unable to write file: %s\n", filename);
            }
        }

        free(ttbin);
    }

    if (formats)
    {
        ptr = formats;
        for (; *ptr; ++ptr)
            free(*ptr);
        free(formats);
    }

    free(files);
}

/* find the firmware version from the downloaded firmware version XML file */
uint32_t decode_latest_firmware_version(char *data)
{
    uint32_t latest_version = 0;
    char *ptr = strstr(data, "<latestVersion>");
    if (!ptr)
        return 0;
    ptr = strstr(ptr + 15, "<Major>");
    if (!ptr)
        return 0;
    latest_version |= atoi(ptr + 7) << 16;
    ptr = strstr(ptr, "<Minor>");
    if (!ptr)
        return 0;
    latest_version |= atoi(ptr + 7) << 8;
    ptr = strstr(ptr, "<Build>");
    if (!ptr)
        return 0;
    latest_version |= atoi(ptr + 7);
    return latest_version;
}

/* find a firmware URL from the downloaded firmware version XML file. Updates the
   provided pointer so that this function can be called repeatedly until it fails,
   to find the complete list of URLs */
char *find_firmware_url(char **data)
{
    char *str;
    char *ptr = strstr(*data, "<URL>");
    if (!ptr)
        return 0;
    *data = ptr + 5;

    ptr = strstr(*data, "</URL>");
    if (!ptr)
        return 0;

    str = malloc(ptr - *data + 1);
    memcpy(str, *data, ptr - *data);
    str[ptr - *data] = 0;

    *data = ptr + 6;

    return str;
}

/* updates either one single file (if id is not zero), or all remaining files (if id is zero) */
int update_firmware_file(libusb_device_handle *device, FIRMWARE_FILE *files, int file_count, uint32_t id)
{
    int i;
    for (i = 0; i < file_count; ++i)
    {
        if (id && (files[i].id != id))
            continue;
        else if (!id && ((files[i].id == FILE_SYSTEM_FIRMWARE) || (files[i].id == FILE_GPS_FIRMWARE) ||
                         (files[i].id == FILE_MANIFEST1)       || (files[i].id == FILE_MANIFEST2)))
        {
            continue;
        }

        /* update the file; even if it fails, still update the next file */
        write_log(0, "Updating firmware file: %08x ... ", files[i].id);
        fflush(stdout);
        if (write_verify_whole_file(device, files[i].id, files[i].download.data, files[i].download.length))
            write_log(0, "Failed\n");
        else
            write_log(0, "Done\n");
    }

    return 0;
}

void do_update_firmware(libusb_device_handle *device)
{
    uint32_t product_id;
    char url[128];
    DOWNLOAD download;
    uint32_t current_version;
    uint32_t latest_version;
    FIRMWARE_FILE *firmware_files = 0;
    int file_count = 0;
    int i;
    uint32_t id;
    char *ptr, *fw_url;

    /* the product id is required to get the correct firmware from TomTom's website */
    product_id = get_product_id(device);
    if (!product_id)
    {
        write_log(1, "Unable to read product ID\n");
        goto cleanup;
    }

    /* download the firmware information file */
    sprintf(url, "http://download.tomtom.com/sweet/fitness/Firmware/%08X/FirmwareVersionConfigV2.xml?timestamp=%d",
        product_id, (int)time(NULL));

    if (download_file(url, &download))
        goto cleanup;
    /* null-terminate the data (yes, we lose the last byte of the file,
       but since it's XML it's only the last closing bracket) */
    download.data[download.length - 1] = 0;

    /* get the current firmware version */
    current_version = get_firmware_version(device);
    if (!current_version)
    {
        write_log(1, "Unable to determine current firmware version\n");
        goto cleanup;
    }

    /* get the latest firmware version */
    latest_version = decode_latest_firmware_version(download.data);
    if (!latest_version)
    {
        write_log(1, "Unable to determine latest firmware version\n");
        goto cleanup;
    }

    /* check to see if we need to update the firmware */
    if (latest_version <= current_version)
        write_log(1, "Current firmware is already at latest version\n");
    else
    {
        write_log(0, "Current Firmware Version: 0x%08x\n", current_version);
        write_log(0, "Latest Firmware Version : 0x%08x\n", latest_version);

        /* download all the firmware files from the server */
        ptr = download.data;
        while (fw_url = find_firmware_url(&ptr))
        {
            /* find the file ID within the file URL */
            char *start = strrchr(fw_url, '/');
            if (!start)
            {
                free(fw_url);
                continue;
            }

            /* allocate space for the new file */
            firmware_files = (FIRMWARE_FILE*)realloc(firmware_files, ++file_count * sizeof(FIRMWARE_FILE));

            firmware_files[file_count - 1].id = strtoul(start + 1, NULL, 0);
            firmware_files[file_count - 1].download.data   = 0;
            firmware_files[file_count - 1].download.length = 0;

            /* create the full URL and download the file */
            sprintf(url, "http://download.tomtom.com/sweet/fitness/Firmware/%08X/%s", product_id, fw_url);
            free(fw_url);
            write_log(0, "Download %s ... ", url);
            fflush(stdout);
            if (download_file(url, &firmware_files[file_count - 1].download))
            {
                write_log(0, "Failed\n");
                goto cleanup;
            }
            else
                write_log(0, "Done\n");
        }

        /* update files in this order: 0x000000f0, 0x00010200, 0x00850000, 0x00850001, others */
        update_firmware_file(device, firmware_files, file_count, FILE_SYSTEM_FIRMWARE);
        send_message_group_1(device);   /* not sure why this is needed */
        update_firmware_file(device, firmware_files, file_count, FILE_GPS_FIRMWARE);
        send_message_group_1(device);   /* not sure why this is needed */
        update_firmware_file(device, firmware_files, file_count, FILE_MANIFEST1);
        update_firmware_file(device, firmware_files, file_count, FILE_MANIFEST2);
        update_firmware_file(device, firmware_files, file_count, 0);

        /* resetting the watch causes the watch to disconnect and
           reconnect. This can take almost 90 seconds to complete */
        reset_watch(device);

        write_log(0, "Firmware updated\n");
    }

    /* get the current BLE version */
    current_version = get_ble_version(device);

    /* find the latest BLE version */
    ptr = strstr(download.data, "<BLE version=\"");
    if (!ptr)
    {
        write_log(1, "Unable to determine latest BLE version\n");
        goto cleanup;
    }
    latest_version = strtoul(ptr + 14, NULL, 0);

    /* check to see if we need to do anything */
    if (latest_version <= current_version)
        write_log(1, "Current BLE firmware is already at latest version\n");
    else
    {
        write_log(0, "Current BLE Firmware Version: %u\n", current_version);
        write_log(0, "Latest BLE Firmware Version : %u\n", latest_version);

        /* find the download URL of the BLE firmware */
        ptr = strstr(ptr, "URL=\"");
        if (!ptr)
        {
            write_log(1, "Unable to determine BLE firmware download URL\n");
            goto cleanup;
        }
        fw_url = ptr + 5;
        ptr = strstr(fw_url, "\"");
        if (!ptr)
        {
            write_log(1, "Unable to determine BLE firmware download URL\n");
            goto cleanup;
        }
        *ptr = 0;

        /* find the BLE firmware file ID */
        ptr = strrchr(fw_url, '/');
        if (!ptr)
        {
            write_log(1, "Unable to determine BLE file ID\n");
            goto cleanup;
        }
        id = strtoul(ptr + 1, NULL, 0);
        if (!id)
        {
            write_log(1, "Unable to determine BLE file ID\n");
            goto cleanup;
        }

        /* create the full URL and download the file */
        sprintf(url, "http://download.tomtom.com/sweet/fitness/Firmware/%08X/%s", product_id, fw_url);

        free(download.data);

        write_log(0, "Download %s ... ", url);
        fflush(stdout);
        if (download_file(url, &download))
        {
            write_log(0, "Failed\n");
            goto cleanup;
        }
        else
            write_log(0, "Done\n");

        /* TODO: wait until the BLE version is updated, then work out how to... */

        write_log(0, "BLE firmware updated\n");
    }

cleanup:
    /* free all the allocated data */
    if (download.data)
        free(download.data);
    while (file_count--)
    {
        if (firmware_files[file_count].download.data)
            free(firmware_files[file_count].download.data);
    }
    if (firmware_files)
        free(firmware_files);
}

void do_get_watch_name(libusb_device_handle *device)
{
    char name[64];
    if (get_watch_name(device, name, sizeof(name)))
        write_log(1, "Unable to get watch name\n");
    write_log(0, "%s\n", name);
}

void do_set_watch_name(libusb_device_handle *device, const char *name)
{
    uint32_t size;
    char *data;
    char *start, *end;
    uint8_t *new_data;
    int diff;
    int len;
    
    data = read_whole_file(device, FILE_PREFERENCES_XML, &size);
    if (!data)
    {
        write_log(1, "Unable to write watch name\n");
        return;
    }

    start = strstr(data, "<watchName>");
    if (!start)
    {
        write_log(1, "Unable to write watch name\n");
        free(data);
        return;
    }

    start += 11;
    end = strstr(start, "</watchName>");
    if (!end)
    {
        write_log(1, "Unable to write watch name\n");
        free(data);
        return;
    }

    /* find the difference in length between the two strings and
       adjust the size of the preferences file accordingly */
    len = strlen(name);
    diff = len - (end - start);
    new_data = malloc(size + diff);

    memcpy(new_data, data, start - data);
    memcpy(new_data + (start - data), name, len);
    memcpy(new_data + (start - data) + len, end, data + size - end + 1);
    size += diff;

    free(data);

    data = update_preferences_modified_time(new_data, &size);

    if (write_verify_whole_file(device, FILE_PREFERENCES_XML, data, size))
        write_log(1, "Unable to write new watch name\n");

    free(data);
}

/* convert an entire string to lowercase */
void tolower_s(char *str)
{
    char *ptr;
    for (ptr = str; *ptr; ++ptr)
        *ptr = tolower(*ptr);
}

char **get_configured_formats(libusb_device_handle *device)
{
    uint32_t size;
    char *data;
    char *start, *end;
    char *start1, *start2;
    char **formats = 0;
    int num_formats = 0;

    /* the preferences XML file contains the export formats */
    data = read_whole_file(device, FILE_PREFERENCES_XML, &size);
    if (!data)
    {
        write_log(1, "Unable to read watch preferences\n");
        goto cleanup;
    }

    /* we don't attempt to process online exporters */
    start = strstr(data, "<offline>");
    if (!start)
    {
        write_log(1, "Unable to read watch preferences\n");
        goto cleanup;
    }
    start += 9;

    start1 = strstr(start, "<export id=\"");
    start2 = strstr(start, "</offline>");

    while (start1 && start2 && (start1 < start2))
    {
        char *ptr;

        start1 += 12;
        end = strchr(start1, '\"');
        if (!end)
            goto cleanup;
        *end++ = 0;

        tolower_s(start1);
        formats = (char**)realloc(formats, ++num_formats * sizeof(char*));
        formats[num_formats - 1] = strdup(start1);

        start1 = strstr(end, "<export id=\"");
        start2 = strstr(end, "</offline>");
    }

    formats = (char**)realloc(formats, ++num_formats * sizeof(char*));
    formats[num_formats - 1] = 0;

cleanup:
    if (data)
        free(data);

    return formats;
}

void do_list_formats(libusb_device_handle *device)
{
    uint32_t size;
    char *data;
    char *start, *end;
    char *start1, *start2;

    /* the preferences XML file contains the export formats */
    data = read_whole_file(device, FILE_PREFERENCES_XML, &size);
    if (!data)
    {
        write_log(1, "Unable to read watch preferences\n");
        goto cleanup;
    }

    /* we don't attempt to process online exporters */
    start = strstr(data, "<offline>");
    if (!start)
    {
        write_log(1, "Unable to read watch preferences\n");
        goto cleanup;
    }
    start += 9;

    start1 = strstr(start, "<export id=\"");
    start2 = strstr(start, "</offline>");

    while (start1 && start2 && (start1 < start2))
    {
        char *ptr;

        start1 += 12;
        end = strchr(start1, '\"');
        if (!end)
            goto cleanup;
        *end++ = 0;
        tolower_s(start1);
        write_log(0, "%s\n", start1);

        start1 = strstr(end, "<export id=\"");
        start2 = strstr(end, "</offline>");
    }

cleanup:
    if (data)
        free(data);
}

void do_set_formats(libusb_device_handle *device, char *formats)
{
    uint32_t size;
    char *data, *new_data;
    char *ptr, *str;
    char *start, *end;
    char *fmts[8];
    int fmt_count = 0;
    int i, len, diff;

    /* scan the formats list to find the recognised formats */
    str = formats;
    do
    {
        ptr = strchr(str, ',');
        if (ptr)
            *ptr = 0;
        if (!strcasecmp(str, "kml") || !strcasecmp(str, "gpx") ||
            !strcasecmp(str, "csv") || !strcasecmp(str, "fit") ||
            !strcasecmp(str, "tcx"))
        {
            fmts[fmt_count++] = str;
        }
        str = ptr + 1;
    }
    while (ptr);

    /* make sure we've got some... */
    if (fmt_count == 0)
    {
        write_log(1, "No valid file formats found\n");
        return;
    }

    /* create the string for the new file formats */
    ptr = str = malloc(45 * fmt_count + 30);
    ptr += sprintf(ptr, "<offline>\n");
    for (i = 0; i < fmt_count; ++i)
        ptr += sprintf(ptr, "            <export id=\"%s\" autoOpen=\"0\"/>\n", fmts[i]);
    ptr += sprintf(ptr, "        </offline>\n");

    /* read the current preferences file */
    data = read_whole_file(device, FILE_PREFERENCES_XML, &size);
    if (!data)
    {
        write_log(1, "Unable to determine current format list\n");
        return;
    }

    len = strlen(str);
    /* look for the section that needs updating */
    start = strstr(data, "<offline>");
    if (!start)
    {
        /* no offline format section, so add one */
        start = strstr(data, "<exporters>");
        if (!start)
        {
            /* oh dear, no exporters section either; give up */
            write_log(1, "Unable to set new format list\n");
            free(data);
            return;
        }
        else
        {
            /* we found an exporters section, so we can add
              the offline section immediately after it */
            start += 11;
            end = start;
        }
    }
    else
    {
        /* find the end of the offline section, so we replace the whole thing */
        end = strstr(start, "</offline>");
        if (!end)
        {
            write_log(1, "Unable to set new format list\n");
            free(data);
            return;
        }
        end += 10;
    }

    /* find the difference in length between the two strings and
       adjust the size of the preferences file accordingly */
    diff = len - (end - start);
    new_data = malloc(size + diff);

    memcpy(new_data, data, start - data);
    memcpy(new_data + (start - data), str, len);
    memcpy(new_data + (start - data) + len, end, data + size - end + 1);
    size += diff;

    free(data);

    data = update_preferences_modified_time(new_data, &size);
    fwrite(data, 1, size, stdout);

    if (write_verify_whole_file(device, FILE_PREFERENCES_XML, data, size))
        write_log(1, "Unable to write new watch name\n");

    free(data);
}

void daemon_watch_operations(libusb_device_handle *device, OPTIONS *options)
{
    char **formats;
    send_startup_message_group(device);

    /* not configurable at this stage - to match the TomTom software:
        1. Download activity files
        2. Update the GPSQuickFix data
        3. Update the system firmware */

    formats = get_configured_formats(device);

    do_get_activities(device, options->activity_store, formats);

    do_update_gps(device);

    do_update_firmware(device);
}

int hotplug_attach_callback(struct libusb_context *ctx, struct libusb_device *dev,
    libusb_hotplug_event event, void *user_data)
{
    libusb_device_handle *device = 0;
    int result;
    OPTIONS *options = (OPTIONS*)user_data;

    write_log(0, "Watch connected...\n");

    if (!(result = libusb_open(dev, &device)))
    {
        int attach_kernel_driver = 0;

        /* detach the kernel HID driver, otherwise we can't do anything */
        if (libusb_kernel_driver_active(device, 0))
        {
            int result = libusb_detach_kernel_driver(device, 0);
            if (!result)
                attach_kernel_driver = 1;
        }
        if(!libusb_claim_interface(device, 0))
        {
            daemon_watch_operations(device, options);

            write_log(0, "Finished watch operations\n");

            libusb_release_interface(device, 0);
        }
        if (attach_kernel_driver)
            libusb_attach_kernel_driver(device, 0);
        libusb_close(device);
    }
    else
        write_log(1, "Unable to open watch device: %d\n", result);

    return 0;
}

void daemonise(const char *user)
{
    pid_t pid, sid;
    struct passwd *pwd;
    struct group *grp;
    FILE *file;
    char *group;

    /* we must be run as root */
    if (getuid() != 0)
    {
        fprintf(stderr, "This daemon must be run as root. The --runas=[USER[:GROUP]] parameter can be\n");
        fprintf(stderr, "used to run as an unprivileged user after daemon initialisation\n");
        exit(1);
    }

    /* find the group in the user name */
    group = strchr(user, ':');
    if (group)
        *(group++) = 0;

    /* perform the first fork */
    pid = fork();
    if (pid < 0)
        exit(1);
    if (pid > 0)
        exit(0);

    /* create a new session with no controlling terminal */
    sid = setsid();
    if (sid < 0)
        _exit(1);

    /* perform the second fork */
    pid = fork();
    if (pid)
        _exit(0);

    /* chdir to the root directory to prevent issues with removing inuse directories */
    if (chdir("/") < 0)
        _exit(1);

    /* set umask to 0 since we don't know what it was before */
    umask(0);

    /* create the pid file */
    file = fopen("/var/run/ttwatch.pid", "w");
    if (file)
    {
        fprintf(file, "%d\n", getpid());
        fclose(file);
    }

    /* retrieve information about the user we want to run as */
    if (user)
    {
        pwd = getpwnam(user);
        if (!pwd)
        {
            perror("getpwnam");
            _exit(1);
        }
    }
    if (group)
    {
        grp = getgrnam(group);
        if (!grp)
        {
            perror("getgrnam");
            _exit(1);
        }
    }

    /* make the log directory... */
    mkdir("/var/log/ttwatch", 0755);
    /* and make our user the owner */
    if (user)
        chown("/var/log/ttwatch", pwd->pw_uid, pwd->pw_gid);

    /* drop privileges to that of the specified user */
    if ((getuid() == 0) && user)
    {
        if (group)
            setgid(grp->gr_gid);
        else
            setgid(pwd->pw_gid);
        setuid(pwd->pw_uid);
    }
}

void help(char *argv[])
{
#ifdef UNSAFE
    write_log(0, "Usage: %s [OPTION]... [FILE]\n", argv[0]);
#else
    write_log(0, "Usage: %s [OPTION]...\n", argv[0]);
#endif
    write_log(0, "Perform various operations with an attached TomTom GPS watch.\n");
    write_log(0, "\n");
    write_log(0, "Mandatory arguments to long options are mandatory for short options too.\n");
    write_log(0, "  -h, --help                 Print this help\n");
    write_log(0, "  -s, --activity-store=PATH Specify an alternate place for storing\n");
    write_log(0, "                               downloaded ttbin activity files\n");
    write_log(0, "  -a, --auto                 Same as \"--update-fw --update-gps --get-activities\"\n");
    write_log(0, "      --daemon               Run the program in daemon mode\n");
    write_log(0, "  -d, --device=NUMBER        Specify which device to use (0, 1, 2, ...)\n");
    write_log(0, "      --devices              List detected USB devices that can be selected.\n");
    write_log(0, "      --get-activities       Downloads and deletes any activity records\n");
    write_log(0, "                               currently stored on the watch\n");
    write_log(0, "      --get-formats          Displays the list of file formats that are\n");
    write_log(0, "                               saved when the watch is automatically processed\n");
    write_log(0, "      --get-name             Displays the current watch name\n");
    write_log(0, "      --get-time             Returns the current GPS time on the watch\n");
#ifdef UNSAFE
    write_log(0, "  -l, --list                 List files currently available on the device\n");
#endif
    write_log(0, "      --packets              Displays the packets being sent/received\n");
    write_log(0, "                               to/from the watch. Only used for debugging\n");
#ifdef UNSAFE
    write_log(0, "  -r, --read=NUMBER          Reads a single file from the device\n");
#endif
    write_log(0, "      --runas=USER[:GROUP]   Run the daemon as the specified user, and\n");
    write_log(0, "                               optionally as the specified group\n");
    write_log(0, "      --set-formats=LIST     Sets the list of file formats that are saved\n");
    write_log(0, "                               when processing activity files\n");
    write_log(0, "      --set-name=STRING      Sets a new watch name\n");
    write_log(0, "      --set-time             Updates the time on the watch (NOT WORKING)\n");
    write_log(0, "      --update-fw            Checks for available firmware updates from\n");
    write_log(0, "                               Tomtom's website and updates the watch if\n");
    write_log(0, "                               newer firmware is found.\n");
    write_log(0, "      --update-gps           Updates the GPSQuickFix data on the watch\n");
    write_log(0, "  -v, --version              Shows firmware version and device identifiers\n");
#ifdef UNSAFE
    write_log(0, "  -w, --write=NUMBER         Writes the specified file on the device\n");
#endif
    write_log(0, "\n");
    write_log(0, "NUMBER is an integer specified in decimal, octal, or hexadecimal form.\n");
#ifdef UNSAFE
    write_log(0, "Read and Write commands require the file ID to be specified. Available\n");
    write_log(0, "IDs can be found using the List command. If a file ID of 0 is specified,\n");
    write_log(0, "the read command will read all available files and store them in files in\n");
    write_log(0, "the current directory. The optional file argument cannot be used in this\n");
    write_log(0, "case. The write command will not accept a file ID of 0.\n");
    write_log(0, "\n");
    write_log(0, "WARNING: DO NOT WRITE ANY FILES UNLESS YOU REALLY KNOW WHAT YOU ARE DOING!\n");
    write_log(0, "\n");
    write_log(0, "The read and write commands have an optional file argument that either\n");
    write_log(0, "contains (write) or receives (read) the file data. If this argument is\n");
    write_log(0, "not specified, stdin (write) or stdout (read) are used instead.\n");
#endif
    write_log(0, "\n");
    write_log(0, "If --activity-store is not specified, \"~/ttwatch\" is used for storing\n");
    write_log(0, "downloaded activity data, with subdirectories of the watch name and\n");
    write_log(0, "activity date, as per the official TomTom software.\n");
    write_log(0, "\n");
    write_log(0, "LIST is a comma-separated list of file formats. Valid formats are:\n");
    write_log(0, "    kml, csv, gpx, tcx and fit.\n");
    write_log(0, "Case is not important, but there must be no spaces or other characters\n");
    write_log(0, "in the list.\n");
    write_log(0, "\n");
    write_log(0, "The program can be run as a daemon, which will automatically download\n");
    write_log(0, "and save activity files, update the GPSQuickFix information and update\n");
    write_log(0, "the watch firmware whenever a watch is connected. The program must be\n");
    write_log(0, "run as root initially, but the --runas parameter can be used to specify\n");
    write_log(0, "a user (and optionally a group) that the program will drop privileges to\n");
    write_log(0, "after it has finished initialising. Without this parameter specified,\n");
    write_log(0, "the program will continue to run as root, which is not recommended for\n");
    write_log(0, "security reasons. Note that this unprivileged user must have access to\n");
    write_log(0, "the USB devices, and write access to the activity store location.\n");
}

int main(int argc, char *argv[])
{
    int opt;
    int option_index = 0;
    int attach_kernel_driver = 0;

    OPTIONS options = {0};

    libusb_device_handle *device;

    struct option long_options[] = {
        { "update-fw",      no_argument,       &options.update_firmware, 1 },
        { "update-gps",     no_argument,       &options.update_gps,      1 },
        { "get-time",       no_argument,       &options.get_time,        1 },
        { "set-time",       no_argument,       &options.set_time,        1 },
        { "get-activities", no_argument,       &options.get_activities,  1 },
        { "packets",        no_argument,       &show_packets,    1 },
        { "devices",        no_argument,       &options.list_devices,    1 },
        { "get-formats",    no_argument,       &options.list_formats,    1 },
        { "get-name",       no_argument,       &options.get_name,        1 },
        { "daemon",         no_argument,       &options.daemon_mode,     1 },
        { "auto",           no_argument,       0, 'a' },
        { "help",           no_argument,       0, 'h' },
        { "version",        no_argument,       0, 'v' },
#ifdef UNSAFE
        { "list",           no_argument,       0, 'l' },
        { "read",           required_argument, 0, 'r' },
        { "write",          required_argument, 0, 'w' },
#endif
        { "device",         required_argument, 0, 'd' },
        { "activity-store", required_argument, 0, 's' },
        { "set-name",       required_argument, 0, 1   },
        { "set-formats",    required_argument, 0, 2   },
        { "runas",          required_argument, 0, 3   },
    };

    /* check the command-line options */
    while ((opt = getopt_long(argc, argv, "a?"
#ifdef UNSAFE
        "lr:w:"
#endif
        "d:s:v", long_options, &option_index)) != -1)
    {
        switch (opt)
        {
        case 1:     /* list formats */
            options.set_name = 1;
            options.watch_name = optarg;
            break;
        case 2:     /* set formats */
            options.set_formats = 1;
            options.formats = optarg;
            break;
        case 3:     /* set daemon user */
            options.run_as = 1;
            options.run_as_user = optarg;
        case 'a':   /* auto mode */
            options.update_firmware = 1;
            options.update_gps      = 1;
            options.get_activities  = 1;
            break;
#ifdef UNSAFE
        case 'l':   /* list files */
            options.list_files = 1;
            break;
        case 'r':   /* read file */
            options.read_file = 1;
            if (optarg)
                options.file_id = strtoul(optarg, NULL, 0);
            break;
        case 'w':   /* write file */
            options.write_file = 1;
            if (optarg)
                options.file_id = strtoul(optarg, NULL, 0);
            break;
#endif
        case 'd':   /* select device */
            options.select_device = 1;
            if (optarg)
                options.device_number = strtoul(optarg, NULL, 10);
            break;
        case 'v':   /* report version information */
            options.show_versions = 1;
            break;
        case 's':   /* activity store */
            if (optarg)
                options.activity_store = optarg;
            break;
        case 'h': /* help */
            help(argv);
            return;
        }
    }

#ifdef UNSAFE
    /* keep track of the file argument if one was provided */
    if (optind <= argc)
        options.file = argv[optind];

    /* make sure we've got compatible command-line options */
    if (options.read_file && options.write_file)
    {
        write_log(1, "Cannot read and write a file at the same time\n");
        return 1;
    }
    if (options.file && !(options.read_file || options.write_file))
    {
        write_log(1, "File argument is only used to read/write files\n");
        return 1;
    }
#endif

    if (!options.activity_store)
    {
        /* find the user's home directory, either from $HOME or from
           looking at the system password database */
        char *home = getenv("HOME");
        if (!home)
        {
            struct passwd *pwd = getpwuid(getuid());
            home = pwd->pw_dir;
        }
        options.activity_store = (char*)malloc(strlen(home) + 9);
        if (options.activity_store)
            sprintf(options.activity_store, "%s/ttwatch", home);
    }

    /* we need to do something, otherwise just show the help */
    if (
#ifdef UNSAFE
        !options.read_file && !options.write_file && !options.list_files &&
#endif
        !options.update_firmware && !options.update_gps && !options.show_versions &&
        !options.get_activities && !options.get_time && !options.set_time &&
        !options.list_devices && !options.get_name && !options.set_name &&
        !options.list_formats && !options.set_formats && !options.daemon_mode)
    {
        help(argv);
        return 0;
    }

    /* if daemon mode is requested ...*/
    if (options.daemon_mode)
    {
        /* become a daemon */
        daemonise(options.run_as ? options.run_as_user : NULL);

        /* we're not a daemon, so open the log file and report that we have started */
        set_log_location(LOG_VAR_LOG);
        write_log(0, "Starting daemon.\n");

        libusb_init(NULL);

        /* setup hot-plug detection so we know when a watch is plugged in */
        if (libusb_has_capability(LIBUSB_CAP_HAS_HOTPLUG))
        {
            int result;
            if (result = libusb_hotplug_register_callback(NULL, LIBUSB_HOTPLUG_EVENT_DEVICE_ARRIVED,
                LIBUSB_HOTPLUG_ENUMERATE, TOMTOM_VENDOR_ID, TOMTOM_PRODUCT_ID,
                LIBUSB_HOTPLUG_MATCH_ANY, hotplug_attach_callback, &options, NULL))
            {
                write_log(1, "Unable to register hotplug callback: %d\n", result);
                _exit(1);
            }

            /* infinite loop - handle events every 10 seconds */
            while (1)
            {
                libusb_handle_events_completed(NULL, NULL);
                usleep(10000);
            }
        }
        else
            write_log(0, "System does not support hotplug notification\n");

        _exit(0);
    }

    libusb_init(NULL);

    /* look for compatible USB devices */
    device = open_usb_device(options.list_devices, options.select_device, options.device_number);
    if (!device)
    {
        if (!options.list_devices)
            write_log(1, "Unable to open USB device\n");
        return 1;
    }
    /* if the device list was requested, exit here */
    if (options.list_devices)
        return 0;

    /* detach the kernel HID driver, otherwise we can't do anything */
    if (libusb_kernel_driver_active(device, 0))
    {
        int result = libusb_detach_kernel_driver(device, 0);
        if (!result)
            attach_kernel_driver = 1;
    }
    if(libusb_claim_interface(device, 0))
    {
        write_log(1, "Unable to claim device interface\n");
        return 1;
    }

    /* this group of messages is always sent at startup */
    send_startup_message_group(device);

    if (options.show_versions)
        show_device_versions(device);

#ifdef UNSAFE
    if (options.list_files)
        show_file_list(device);

    if (options.read_file)
    {
        if (options.file_id == 0)
            read_all_files(device);
        else
        {
            FILE *f;
            if (!options.file)
                f = stdout;
            else
                f = fopen(options.file, "w");
            if (!f)
                write_log(1, "Unable to open file: %s", options.file);
            else
            {
                do_read_file(device, options.file_id, f);
                if (f != stdout)
                    fclose(f);
            }
        }
    }

    if (options.write_file)
    {
        if (options.file_id == 0)
            write_log(1, "File ID must be non-zero when writing a file\n");
        else
        {
            FILE *f;
            if (!options.file)
                f = stdin;
            else
                f = fopen(options.file, "r");
            if (!f)
                write_log(1, "Unable to open file: %s", options.file);
            else
            {
                do_write_file(device, options.file_id, f);
                if (f != stdin)
                    fclose(f);
            }
        }
    }
#endif

    if (options.get_time)
        do_get_time(device);

    if (options.set_time)
        do_set_time(device);

    if (options.get_activities)
        do_get_activities(device, options.activity_store, NULL);

    if (options.update_gps)
        do_update_gps(device);

    if (options.update_firmware)
        do_update_firmware(device);

    if (options.get_name)
        do_get_watch_name(device);

    if (options.set_name)
        do_set_watch_name(device, options.watch_name);

    if (options.list_formats)
        do_list_formats(device);

    if (options.set_formats)
        do_set_formats(device, options.formats);

    libusb_release_interface(device, 0);

    if (attach_kernel_driver)
        libusb_attach_kernel_driver(device, 0);
    libusb_close(device);

    libusb_exit(NULL);

    return 0;
}

