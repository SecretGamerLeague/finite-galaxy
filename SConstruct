import os

# Load environment variables, including some that should be renamed.
env = Environment(ENV = os.environ)
if 'CXXFLAGS' in os.environ:
	env.Append(CCFLAGS = os.environ['CXXFLAGS'])
if 'LDFLAGS' in os.environ:
	env.Append(LINKFLAGS = os.environ['LDFLAGS'])

# The Steam runtime has an out-of-date libstdc++, so link it in statically:
if 'SCHROOT_CHROOT_NAME' in os.environ and 'steamrt' in os.environ['SCHROOT_CHROOT_NAME']:
	env.Append(LINKFLAGS = ["-static-libstdc++"])

opts = Variables()
opts.Add(PathVariable("PREFIX", "Directory to install under", "/usr/local", PathVariable.PathIsDirCreate))
opts.Add(PathVariable("DESTDIR", "Destination root directory", "", PathVariable.PathAccept))
opts.Add(EnumVariable("mode", "Compilation mode", "release", allowed_values=("release", "debug", "profile")))
opts.Add(PathVariable("BUILDDIR", "Build directory", "build", PathVariable.PathIsDirCreate))
opts.Update(env)

Help(opts.GenerateHelpText(env))

flags = ["-std=c++11", "-Wall"]
if env["mode"] != "debug":
	flags += ["-O3"]
if env["mode"] == "debug":
	flags += ["-g"]
if env["mode"] == "profile":
	flags += ["-pg"]
	env.Append(LINKFLAGS = ["-pg"])

# Required build flags. If you want to use SSE optimization, you can turn on
# -msse3 or (if just building for your own computer) -march=native.
env.Append(CCFLAGS = flags)
env.Append(LIBS = [
	"SDL2",
	"png",
	"jpeg",
	"GL",
	"GLEW",
	"openal",
	"pthread"
]);
# libmad is not in the Steam runtime, so link it statically:
if 'SCHROOT_CHROOT_NAME' in os.environ and 'steamrt_scout_i386' in os.environ['SCHROOT_CHROOT_NAME']:
	env.Append(LIBS = File("/usr/lib/i386-linux-gnu/libmad.a"))
elif 'SCHROOT_CHROOT_NAME' in os.environ and 'steamrt_scout_amd64' in os.environ['SCHROOT_CHROOT_NAME']:
	env.Append(LIBS = File("/usr/lib/x86_64-linux-gnu/libmad.a"))
else:
	env.Append(LIBS = "mad")


buildDirectory = env["BUILDDIR"] + "/" + env["mode"]
VariantDir(buildDirectory, "code", duplicate = 0)

sky = env.Program("finite-galaxy", Glob(buildDirectory + "/*.cpp"))


# Install the binary:
env.Install("$DESTDIR$PREFIX/games", sky)

# Install the desktop file:
env.Install("$DESTDIR$PREFIX/share/applications", "finite-galaxy.desktop")

# Install app center metadata:
env.Install("$DESTDIR$PREFIX/share/appdata", "finite-galaxy.appdata.xml")

# Install icons, keeping track of all the paths.
# Most Ubuntu apps supply 16, 22, 24, 32, 48, and 256, and sometimes others.
sizes = ["16", "22", "24", "32", "48", "64", "128", "256", "512"]
icons = []
for size in sizes:
	destination = "$DESTDIR$PREFIX/share/icons/hicolor/" + size + "/apps/finite-galaxy.png"
	icons.append(destination)
	env.InstallAs(destination, "icons/" + size + ".png")

# If any of those icons changed, also update the cache.
# Do not update the cache if we're not installing into "usr".
# (For example, this "install" may actually be creating a Debian package.)
if env.get("PREFIX").startswith("/usr/"):
	env.Command(
		[],
		icons,
		"gtk-update-icon-cache -t $DESTDIR$PREFIX/share/icons/hicolor/")

# Install the man page.
env.Command(
	"$DESTDIR$PREFIX/share/man/man6/finite-galaxy.6.gz",
	"finite-galaxy.6",
	"gzip -c $SOURCE > $TARGET")

# Install the data files.
def RecursiveInstall(env, target, source):
	rootIndex = len(env.Dir(source).abspath) + 1
	for node in env.Glob(os.path.join(source, '*')):
		if node.isdir():
			name = node.abspath[rootIndex:]
			RecursiveInstall(env, os.path.join(target, name), node.abspath)
		else:
			env.Install(target, node)
RecursiveInstall(env, "$DESTDIR$PREFIX/share/games/finite-galaxy/data", "data")
RecursiveInstall(env, "$DESTDIR$PREFIX/share/games/finite-galaxy/images", "images")
RecursiveInstall(env, "$DESTDIR$PREFIX/share/games/finite-galaxy/sounds", "sounds")
env.Install("$DESTDIR$PREFIX/share/games/finite-galaxy", "credits.txt")
env.Install("$DESTDIR$PREFIX/share/games/finite-galaxy", "keys.txt")

# Make the word "install" in the command line do an installation.
env.Alias("install", "$DESTDIR$PREFIX")

