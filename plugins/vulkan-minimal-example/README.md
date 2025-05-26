# Wayfire Example Plugin

## Building

```
$ git clone https://github.com/soreau/wayfire-example-plugin
$ cd wayfire-example-plugin
$ meson setup build --prefix=/usr
$ ninja -C build
# ninja -C build install
```

## Testing

A message will be printed to the wayfire log upon successful load of the plugin.

`II 08-08-24 09:39:45.152 - [src/example-plugin.cpp:33] Hello World!`

A message will be printed to the wayfire log on plugin unload.

`II 08-08-24 09:39:47.524 - [src/example-plugin.cpp:38] Goodbye, Cruel World..`

## Renaming

If you are planning to write a wayfire plugin using this as a template,
the first thing you probably want to do is rename the plugin. This can
be a bit involving, but since this plugin is simple, we can distill the
key parts.

First, rename the git directory or clone it with this command:

`$ git clone https://github.com/soreau/wayfire-example-plugin my-plugin`
This should download the source into a directory called `my-plugin`.

`$ cd my-plugin`

Next, we'll want to rename the relevant files and contents.

Rename metadata/example-plugin.xml to metadata/my-plugin.xml:

`$ mv metadata/example-plugin.xml metadata/my-plugin.xml`

Rename src/example-plugin.cpp to src/my-plugin.cpp:

`$ mv src/example-plugin.cpp src/my-plugin.cpp`

Edit `meson.build` and change the instance of `example-plugin` to `my-plugin`.

Edit `metadata/meson.build` and change the instance of `example-plugin.xml` to `my-plugin.xml`.

Edit `src/meson.build` and change the instance of `example-plugin.cpp` to `my-plugin.cpp`.

Edit `metadata/my-plugin.xml` and change the instance of `example-plugin` to `my-plugin` and `Example Plugin` to `My Plugin`.

Edit `src/my-plugin.cpp` and change all instances of `wayfire_example_plugin` to `wayfire_my_plugin`.

Finally, remove the build directory, rebuild and test.

## Conclusion

Every wayfire plugin is built into a shared object. The init() function is called when loaded
and the fini() function is called just before unload. When reading the source of other plugins
to pick out bits to do what you want, you can start by finding the init() and fini() functions
to see the setup and teardown. This gives a big clue on where to start writing. Happy hacking!
