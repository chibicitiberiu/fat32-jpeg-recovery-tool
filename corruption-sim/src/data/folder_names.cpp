/*
 * folder_names.cpp - Embedded dictionaries for realistic folder/file names.
 */
#include "corrsim.h"

static const std::vector<std::string> g_camera = {
    "DCIM", "100CANON", "101CANON", "102CANON", "100NIKON", "101NIKON",
    "100OLYMP", "101OLYMP", "100MEDIA", "101MEDIA", "102_PANA", "103_PANA",
    "100APPLE", "101APPLE", "100GOPRO", "100SAMSUNG", "100FUJI",
    "MISC", "PRIVATE", "AVCHD"
};

static const std::vector<std::string> g_english = {
    "Photos", "Vacation", "Family", "Wedding", "Birthday", "Christmas",
    "Summer 2024", "Summer 2023", "Winter 2024", "Trip to Rome",
    "Trip to Paris", "Trip to Barcelona", "Beach Trip", "Camping",
    "New Year", "Halloween", "Easter", "Graduation",
    "Work Documents", "Screenshots", "Downloads", "Camera Roll",
    "Favorites", "Archive", "Backup", "Old Photos",
    "Kids", "Baby", "Pets", "Garden", "Home Renovation",
    "School", "Party", "Concert", "Food",
    "Landscape", "Portraits", "Selfies", "Panoramas",
    "Videos", "Edited", "To Print", "Shared",
    "Mom and Dad", "Grandparents", "Friends", "Neighbors"
};

static const std::vector<std::string> g_romanian = {
    "Poze", "Vacanta", "Familie", "Nunta", "Craciun",
    "Revelion", "Concediu", "Drumetie", "Excursie",
    "Prieteni", "Amintiri", "Sarbatori", "Poze vechi",
    "Album foto", "Documente", "Copii", "Munte",
    "Mare", "Paste", "Anul Nou", "Botez",
    "Absolvire", "Petrecere", "Gradina",
    "Animale", "Calatorie", "Aventura",
    "Bunici", "Neamuri", "Scoala",
    "Poze telefon", "De printat", "Oras"
};

static const std::vector<std::string> g_file_prefix = {
    "DSC_", "IMG_", "DSCN", "DSCF", "P101", "P102",
    "SAM_", "GOPR", "_MG_", "DJI_", "PANO"
};

static const std::vector<std::string> g_file_descriptive = {
    "Birthday Party", "Sunset at the beach", "Family dinner",
    "Christmas morning", "First day of school", "Wedding ceremony",
    "Lake view", "Mountain hike", "City skyline",
    "Group photo", "Garden flowers", "Snow day",
    "Summer BBQ", "Graduation day", "New apartment",
    "Road trip", "Camping fire", "Sunrise",
    "Cat napping", "Dog playing", "Baby steps",
    "Holiday decorations", "Fireworks", "Rainy day",
    "Autumn leaves", "Spring blossoms", "Night sky",
    "Old church", "Market day", "River walk",
    "Apus de soare", "La munte", "Pe plaja",
    "Zi de nastere", "Masa de Craciun", "La bunici",
    "In parc", "Prieteni vechi", "Seara de vara"
};

const std::vector<std::string> &folder_names_camera()   { return g_camera; }
const std::vector<std::string> &folder_names_english()  { return g_english; }
const std::vector<std::string> &folder_names_romanian() { return g_romanian; }
const std::vector<std::string> &file_name_prefixes()    { return g_file_prefix; }
const std::vector<std::string> &file_name_descriptive() { return g_file_descriptive; }
