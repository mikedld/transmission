// This file Copyright © Mnemosyne LLC.
// It may be used under GPLv2 (SPDX: GPL-2.0-only), GPLv3 (SPDX: GPL-3.0-only),
// or any future license endorsed by Mnemosyne LLC.
// License text can be found in the licenses/ folder.

#include "IPToLocation.h"

#include <curl/curl.h>
#include <libdeflate.h>
#include <maxminddb.h>

#include <glibmm/datetime.h>
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>

#include <chrono>
#include <cstddef>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>

// Function to decompress the mmdb.gz file after download
void decompress_gz_file(std::string filename)
{
    std::ifstream gz_file(filename, std::ios::binary);
    if (!gz_file)
    {
        std::cerr << "Error opening " + filename + " to perform decompression\n";
        return;
    }

    // Read the entire .gz file into a buffer
    std::vector<char> gz_data((std::istreambuf_iterator<char>(gz_file)), std::istreambuf_iterator<char>());

    // Initialize decompressor
    libdeflate_decompressor* decompressor = libdeflate_alloc_decompressor();
    if (decompressor == nullptr)
    {
        std::cerr << "libdeflate: Failed to create decompressor\n";
        return;
    }

    // Prepare buffer to hold decompressed data (estimate maximum decompressed size)
    size_t const decompressed_size = gz_data.size() * 4;
    std::vector<char> decompressed_data(decompressed_size);

    // Perform the decompression
    size_t decompressed_len = 0;
    int const result = libdeflate_gzip_decompress(
        decompressor,
        gz_data.data(),
        gz_data.size(),
        decompressed_data.data(),
        decompressed_size,
        &decompressed_len);

    if (result != LIBDEFLATE_SUCCESS)
    {
        std::cerr << "libdeflate: Decompression of " + filename + " failed\n";
    }
    else
    {
        // Write decompressed data to the output file
        filename.erase(filename.size() - 3, 3); // erase ".gz" suffix
        std::ofstream output_file(filename, std::ios::binary);
        if (!output_file)
        {
            std::cerr << "Error opening " + filename + " to save decompression result\n";
        }
        else
        {
            output_file.write(decompressed_data.data(), decompressed_len);
        }
    }
    libdeflate_free_decompressor(decompressor);
}

// Function to download and update the mmdb file
// Free database with monthly updates from <https://db-ip.com/>
void maintain_mmdb_file(std::string const& mmdb_file)
{
    // UTC year and month
    using namespace std::chrono;
    auto date_time = Glib::DateTime::create_now_utc(system_clock::to_time_t(system_clock::now()));
    std::string const year = std::to_string(date_time.get_year());
    std::string month = std::to_string(date_time.get_month());
    if (month.size() == 1)
    {
        month = "0" + month; // pad
    }

    std::string url;
    if (!Glib::file_test(mmdb_file, Glib::FileTest::EXISTS))
    {
        url = "https://download.db-ip.com/free/dbip-city-lite-" + year + "-" + month + ".mmdb.gz";
    }
    else
    {
        MMDB_s mmdb;
        int const status = MMDB_open(mmdb_file.c_str(), MMDB_MODE_MMAP, &mmdb);
        if (MMDB_SUCCESS != status)
        {
            std::cerr << "libmaxminddb: Error opening database file (" << mmdb_file << ", " << MMDB_strerror(status) << ")\n";
            url = "https://download.db-ip.com/free/dbip-city-lite-" + year + "-" + month + ".mmdb.gz";
        }
        else
        {
            // Existing database file's year and month
            time_t const build_epoch = mmdb.metadata.build_epoch;
            date_time = Glib::DateTime::create_now_utc(build_epoch);
            std::string const mmdb_year = std::to_string(date_time.get_year());
            std::string mmdb_month = std::to_string(date_time.get_month());
            if (mmdb_month.size() == 1)
            {
                mmdb_month = "0" + mmdb_month; // pad
            }
            if (month != mmdb_month || year != mmdb_year)
            {
                url = "https://download.db-ip.com/free/dbip-city-lite-" + year + "-" + month + ".mmdb.gz";
            }
        }
        MMDB_close(&mmdb);
    }

    if (url.empty()) // no need for a download
    {
        return;
    }

    // Download mmdb.gz file
    CURL* curl = curl_easy_init();
    if (curl != nullptr)
    {
        FILE* fp = std::fopen((mmdb_file + ".gz").c_str(), "wb");
        if (fp == nullptr)
        {
            std::cerr << "Error opening " + mmdb_file + ".gz to download MaxMind database\n";
            return;
        }

        curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
        CURLcode const res = curl_easy_perform(curl);
        curl_easy_cleanup(curl); // always cleanup

        if (std::fclose(fp) != 0)
        {
            std::cerr << "Error closing " + mmdb_file + ".gz\n";
            return;
        }

        if (res != CURLE_OK)
        {
            std::cerr << "libcurl: Error downloading " + url + " - " << curl_easy_strerror(res) << '\n';
            return;
        }

        // Decompress mmdb.gz file
        decompress_gz_file(mmdb_file + ".gz");
        if (remove((mmdb_file + ".gz").c_str()) != 0)
        {
            std::perror(("Error deleting " + mmdb_file + ".gz").c_str());
        }
    }
}

std::string get_location_from_ip(std::string const& ip)
{
    // Database file path
    std::vector<std::string> path;
    path.emplace_back(Glib::get_home_dir());
    path.emplace_back("dbip-city-lite.mmdb");
    std::string const mmdb_file = Glib::build_filename(path);

    maintain_mmdb_file(mmdb_file);

    // Compute location from IP
    MMDB_s mmdb;
    int status = MMDB_open(mmdb_file.c_str(), MMDB_MODE_MMAP, &mmdb);
    if (MMDB_SUCCESS != status)
    {
        std::cerr << "libmaxminddb: Error opening database file (" + mmdb_file + ", " + MMDB_strerror(status) + ")\n";
        return "";
    }
    int gai_error = 0;
    int mmdb_error = 0;
    MMDB_lookup_result_s result = MMDB_lookup_string(&mmdb, ip.c_str(), &gai_error, &mmdb_error);
    if (0 != gai_error || MMDB_SUCCESS != mmdb_error || !result.found_entry)
    {
        std::cerr << "libmaxminddb: Error looking up IP " + ip + "\n";
        MMDB_close(&mmdb);
        return "";
    }
    std::string location;
    MMDB_entry_data_s entry_data;
    status = MMDB_get_value(&result.entry, &entry_data, "city", "names", "en", NULL);
    if (MMDB_SUCCESS != status || !entry_data.has_data)
    {
        std::cerr << "libmaxminddb: Error getting value of city for IP " + ip + "\n";
    }
    else
    {
        location.append(entry_data.utf8_string, entry_data.data_size);
        location += ", ";
    }
    status = MMDB_get_value(&result.entry, &entry_data, "country", "names", "en", NULL);
    if (MMDB_SUCCESS != status || !entry_data.has_data)
    {
        std::cerr << "libmaxminddb: Error getting value of country for IP " + ip + "\n";
        MMDB_close(&mmdb);
        return "";
    }
    location.append(entry_data.utf8_string, entry_data.data_size);
    MMDB_close(&mmdb);
    return location;
}
