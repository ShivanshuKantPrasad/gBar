#include "SNI.h"
#include "Log.h"
#include "Widget.h"

#include <sni-watcher.h>
#include <sni-item.h>
#include <gio/gio.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>

#include <fstream>

namespace SNI
{
    sniWatcher* watcherSkeleton;
    guint watcherID;
    GDBusConnection* dbusConnection = nullptr;

    guint hostID;

    struct Item
    {
        std::string name;
        std::string object;
        size_t w;
        size_t h;
        uint8_t* iconData = nullptr;
    };
    std::vector<Item> items;

    // Gtk stuff, TODO: Allow more than one instance
    // Simply removing the gtk_drawing_areas doesn't trigger proper redrawing
    //   HACK: Make an outer permanent and an inner box, which will be deleted and readded
    Widget* parentBox;
    Widget* iconBox;

    // SNI implements the GTK-Thingies itself internally
    static void InvalidateWidget()
    {
        parentBox->RemoveChild(iconBox);

        auto container = Widget::Create<Box>();
        iconBox = container.get();
        for (auto& item : items)
        {
            if (item.iconData)
            {
                auto texture = Widget::Create<Texture>();
                texture->SetHorizontalTransform({32, true, Alignment::Fill});
                texture->SetBuf(item.w, item.h, item.iconData);
                iconBox->AddChild(std::move(texture));
            }
        }
        parentBox->AddChild(std::move(container));
    }

    void WidgetSNI(Widget& parent)
    {
        // Add parent box
        auto box = Widget::Create<Box>();
        auto container = Widget::Create<Box>();
        iconBox = container.get();
        parentBox = box.get();
        InvalidateWidget();
        box->AddChild(std::move(container));
        parent.AddChild(std::move(box));
    }

    static Item CreateItem(std::string&& name, std::string&& object)
    {
        Item item{};
        item.name = name;
        item.object = object;
        auto getProperty = [&](const char* prop) -> GVariant*
        {
            GError* err = nullptr;
            GVariant* params[2];
            params[0] = g_variant_new_string("org.kde.StatusNotifierItem");
            params[1] = g_variant_new_string(prop);
            GVariant* param = g_variant_new_tuple(params, 2);
            GVariant* res = g_dbus_connection_call_sync(dbusConnection, name.c_str(), object.c_str(), "org.freedesktop.DBus.Properties", "Get", param,
                                                        G_VARIANT_TYPE("(v)"), G_DBUS_CALL_FLAGS_NONE, -1, nullptr, &err);
            if (err)
            {
                g_error_free(err);
                return nullptr;
            }
            // There's probably a better method than to use 3 variants
            // g_variant_unref(params[0]);
            // g_variant_unref(params[1]);
            // g_variant_unref(param);
            return res;
        };
        GVariant* iconPixmap = getProperty("IconPixmap");
        if (iconPixmap)
        {
            // Only get first item
            GVariant* arr = nullptr;
            g_variant_get(iconPixmap, "(v)", &arr);

            GVariantIter* arrIter = nullptr;
            g_variant_get(arr, "a(iiay)", &arrIter);

            int width;
            int height;
            GVariantIter* data = nullptr;
            g_variant_iter_next(arrIter, "(iiay)", &width, &height, &data);

            LOG(width);
            LOG(height);
            item.w = width;
            item.h = height;
            item.iconData = new uint8_t[width * height * 4];

            uint8_t px = 0;
            int i = 0;
            while (g_variant_iter_next(data, "y", &px))
            {
                item.iconData[i] = px;
                i++;
            }
            for (int i = 0; i < width * height; i++)
            {
                struct Px
                {
                    // This should be bgra...
                    // Since source is ARGB32 in network order(=big-endian)
                    // and x86 Linux is little-endian, we *should* swap b and r...
                    uint8_t a, r, g, b;
                };
                Px& pixel = ((Px*)item.iconData)[i];
                // Swap to create rgba
                pixel = {pixel.r, pixel.g, pixel.b, pixel.a};
            }

            g_variant_iter_free(data);
            g_variant_iter_free(arrIter);
            g_variant_unref(arr);
            g_variant_unref(iconPixmap);
        }
        else
        {
            // Get icon theme path
            GVariant* themePathVariant = getProperty("IconThemePath"); // Not defined by freedesktop, I think ayatana does this...
            GVariant* iconNameVariant = getProperty("IconName");

            std::string iconPath;
            if (themePathVariant && iconNameVariant)
            {
                // Why GLib?
                GVariant* themePathStr = nullptr;
                g_variant_get(themePathVariant, "(v)", &themePathStr);
                GVariant* iconNameStr = nullptr;
                g_variant_get(iconNameVariant, "(v)", &iconNameStr);

                const char* themePath = g_variant_get_string(themePathStr, nullptr);
                const char* iconName = g_variant_get_string(iconNameStr, nullptr);
                iconPath = std::string(themePath) + "/" + iconName + ".png"; // TODO: Find out if this is always png

                g_variant_unref(themePathVariant);
                g_variant_unref(themePathStr);
                g_variant_unref(iconNameVariant);
                g_variant_unref(iconNameStr);
            }
            else if (iconNameVariant)
            {
                GVariant* iconNameStr = nullptr;
                g_variant_get(iconNameVariant, "(v)", &iconNameStr);

                const char* iconName = g_variant_get_string(iconNameStr, nullptr);
                iconPath = std::string(iconName);

                g_variant_unref(iconNameVariant);
                g_variant_unref(iconNameStr);
            }
            else
            {
                LOG("SNI: Unknown path!");
                return item;
            }

            int width, height, channels;
            stbi_uc* pixels = stbi_load(iconPath.c_str(), &width, &height, &channels, STBI_rgb_alpha);
            if (!pixels)
            {
                LOG("SNI: Cannot open " << iconPath);
                return item;
            }
            item.w = width;
            item.h = height;
            item.iconData = new uint8_t[width * height * 4];
            // Already rgba32
            memcpy(item.iconData, pixels, width * height * 4);
            stbi_image_free(pixels);
        }
        return item;
    }

    // Methods
    static void RegisterItem(sniWatcher*, GDBusMethodInvocation* invocation, const char* service)
    {
        std::string name;
        std::string object;
        if (strncmp(service, "/", 1) == 0)
        {
            // service is object (used by e.g. ayatana -> steam, discord)
            object = service;
            name = g_dbus_method_invocation_get_sender(invocation);
        }
        else
        {
            // service is bus name (used by e.g. Telegram)
            name = service;
            object = "/StatusNotifierItem";
        }
        auto it = std::find_if(items.begin(), items.end(), [&](const Item& item)
                {
                    return item.name == name && item.object == object;
                });
        if (it != items.end())
        {
            LOG("Rejecting " << name << " " << object);
            return;
        }
        // TODO: Add mechanism to remove items
        LOG("SNI: Registered Item " << name << " " << object);
        Item item = CreateItem(std::move(name), std::move(object));
        items.push_back(std::move(item));
        InvalidateWidget();
    }
    static void RegisterHost(sniWatcher*, GDBusMethodInvocation*, const char*)
    {
        LOG("TODO: Implement RegisterHost!");
    }

    // Signals
    static void ItemRegistered(sniWatcher*, const char*, void*)
    {
        // Don't care, since watcher and host will always be from gBar (at least for now)
    }
    static void ItemUnregistered(sniWatcher*, const char*, void*)
    {
        // Don't care, since watcher and host will always be from gBar (at least for now)
    }

    void Init()
    {
        auto busAcquired = [](GDBusConnection* connection, const char*, void*)
        {
            GError* err = nullptr;
            g_dbus_interface_skeleton_export((GDBusInterfaceSkeleton*)watcherSkeleton, connection, "/StatusNotifierWatcher", &err);
            if (err)
            {
                LOG("Failed to connect to dbus! Error: " << err->message);
                g_error_free(err);
                return;
            }
            dbusConnection = connection;

            // Connect methods and signals
            g_signal_connect(watcherSkeleton, "handle-register-status-notifier-item", G_CALLBACK(RegisterItem), nullptr);
            g_signal_connect(watcherSkeleton, "handle-register-status-notifier-host", G_CALLBACK(RegisterHost), nullptr);

            g_signal_connect(watcherSkeleton, "status-notifier-item-registered", G_CALLBACK(ItemRegistered), nullptr);
            g_signal_connect(watcherSkeleton, "status-notifier-item-unregistered", G_CALLBACK(ItemUnregistered), nullptr);

            // Host is always available
            sni_watcher_set_is_status_notifier_host_registered(watcherSkeleton, true);
        };
        auto emptyCallback = [](GDBusConnection*, const char*, void*) {};
        auto lostName = [](GDBusConnection*, const char*, void*)
        {
            LOG("Lost name!");
        };
        auto flags = G_BUS_NAME_OWNER_FLAGS_REPLACE | G_BUS_NAME_OWNER_FLAGS_ALLOW_REPLACEMENT;
        g_bus_own_name(G_BUS_TYPE_SESSION, "org.kde.StatusNotifierWatcher", (GBusNameOwnerFlags)flags, +busAcquired, +emptyCallback, +lostName,
                       nullptr, nullptr);
        watcherSkeleton = sni_watcher_skeleton_new();

        std::string hostName = "org.kde.StatusNotifierHost-" + std::to_string(getpid());
        g_bus_own_name(G_BUS_TYPE_SESSION, hostName.c_str(), (GBusNameOwnerFlags)flags, +emptyCallback, +emptyCallback, +emptyCallback, nullptr,
                       nullptr);
    }

    void Shutdown() {}
}