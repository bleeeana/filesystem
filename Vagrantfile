Vagrant.configure("2") do |config|
  config.vm.box = ENV.fetch("FS_BOX", "cloud-image/debian-13")
  config.vm.hostname = "fs-dev"

  config.vm.synced_folder ".", "/vagrant"

  config.vm.provider "virtualbox" do |vb|
    vb.name = "fs-dev"
    vb.memory = 2048
    vb.cpus = 2
  end

  config.vm.provision "shell", inline: <<-SHELL
    set -eu

    apt-get update
    apt-get install -y build-essential gcc make kmod util-linux gdb ca-certificates linux-headers-amd64

    mkdir -p /mnt
    chmod 777 /mnt
    CURRENT_KERNEL=$(uname -r)
    if [ ! -e "/lib/modules/$CURRENT_KERNEL/build" ]; then
      reboot
    fi
  SHELL
end
