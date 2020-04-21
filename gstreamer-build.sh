#!/bin/bash
set -e

echo "Plese open another terminal and type the following: "
echo $$
echo " pkttyagent --process" $$
echo "Press any key to continue"
while [ true ] ; do
read -t 500 -n 1
if [ $? = 0 ] ; then
echo "going on"
else
echo "waiting for the keypress"
fi


# Create a log file of the build as well as displaying the build on the tty as it runs
exec > >(tee build_gstreamer.log)
exec 2>&1

# Update and Upgrade the Pi, otherwise the build may fail due to inconsistencies
sudo apt-get update && sudo apt-get upgrade -y

# Get the required libraries
sudo apt-get install -y             git python3 python3-pip python3-setuptools python3-wheel ninja-build\
                                    build-essential autotools-dev automake autoconf \
                                    libtool autopoint libxml2-dev zlib1g-dev libglib2.0-dev \
                                    pkg-config bison flex python3 git gtk-doc-tools libasound2-dev \
                                    libgudev-1.0-dev libxt-dev libvorbis-dev libcdparanoia-dev \
                                    libpango1.0-dev libtheora-dev libvisual-0.4-dev iso-codes \
                                    libgtk-3-dev libraw1394-dev libiec61883-dev libavc1394-dev \
                                    libv4l-dev libcairo2-dev libcaca-dev libspeex-dev libpng-dev \
                                    libshout3-dev libjpeg-dev libaa1-dev libflac-dev libdv4-dev \
                                    libtag1-dev libwavpack-dev libpulse-dev libsoup2.4-dev libbz2-dev \
                                    libcdaudio-dev libdc1394-22-dev ladspa-sdk libass-dev \
                                    libcurl4-gnutls-dev libdca-dev libdvdnav-dev \
                                    libexempi-dev libexif-dev libfaad-dev libgme-dev libgsm1-dev \
                                    libiptcdata0-dev libkate-dev libmimic-dev libmms-dev \
                                    libmodplug-dev libmpcdec-dev libofa0-dev libopus-dev \
                                    librsvg2-dev librtmp-dev libschroedinger-dev libslv2-dev \
                                    libsndfile1-dev libsoundtouch-dev libspandsp-dev libx11-dev \
                                    libxvidcore-dev libzbar-dev libzvbi-dev liba52-0.7.4-dev \
                                    libcdio-dev libdvdread-dev libmad0-dev libmp3lame-dev \
                                    libmpeg2-4-dev libopencore-amrnb-dev libopencore-amrwb-dev \
                                    libsidplay1-dev libtwolame-dev libx264-dev libusb-1.0 \
                                    python-gi-dev yasm python3-dev libgirepository1.0-dev \
                                    #libgstreamer1.0-0

sudo pip3 install meson
PATH=/home/pi/.local/bin:$PATH

# get the repos if they're not already there
cd $HOME
[ ! -d src ] && mkdir src
cd src
[ ! -d gstreamer ] && mkdir gstreamer
cd gstreamer

#get repos if they are not there yet
[ ! -d gstreamer ] && git clone https://github.com/GStreamer/gstreamer.git
[ ! -d gst-plugins-base ] && git clone https://github.com/GStreamer/gst-plugins-base.git
[ ! -d gst-plugins-good ] && git clone https://github.com/GStreamer/gst-plugins-good.git
[ ! -d gst-plugins-bad ] && git clone https://github.com/GStreamer/gst-plugins-bad.git
[ ! -d gst-plugins-ugly ] && git clone https://github.com/GStreamer/gst-plugins-ugly.git
[ ! -d gst-omx ] && git clone https://github.com/GStreamer/gst-omx.git

#export LD_LIBRARY_PATH=/usr/local/lib/
cd gstreamer
meson builddir && cd builddir
ninja
echo "Please Autenticate"
meson install --no-rebuild

cd gst-plugins-base
meson builddir && cd builddir
ninja
echo "Please Autenticate"
meson install --no-rebuild
cd ../..

cd gst-plugins-good
meson builddir && cd builddir
ninja
echo "Please Autenticate"
meson install --no-rebuild
cd ../..

cd gst-plugins-ugly
meson builddir && cd builddir
ninja
echo "Please Autenticate"
meson install --no-rebuild
cd ../..

cd gst-plugins-bad
meson builddir && cd builddir
ninja
echo "Please Autenticate"
meson install --no-rebuild
cd ../..

# omx support
#cd gst-omx
#meson builddir && cd builddir
#ninja
#meson install --no-rebuild
#cd ../..


git clone https://github.com/TimoErdmann/gst-rtsp-server.git
cd gst-rtsp-server
git checkout my_webcam2.0
meson builddir && cd builddir
ninja
echo "Please Autenticate"
meson install --no-rebuild

cp examples/webcam_shared /home/pi/
cp ../webcam.service /etc/systemd/system/
sudo systemctl enable webcam.service


done
