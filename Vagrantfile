# -*- mode: ruby -*-
# vi: set ft=ruby :

Vagrant.configure("2") do |config|
  config.vm.box = ENV.fetch("SIMPLEFS_BOX", "cloud-image/debian-13")
  config.vm.hostname = "simplefs-dev"

  config.vm.synced_folder ".", "/vagrant"

  config.vm.provider "virtualbox" do |vb|
    vb.name = "simplefs-dev"
    vb.memory = 2048
    vb.cpus = 2
  end

  config.vm.provision "shell", inline: <<-SHELL
    set -eu

    apt-get update
    apt-get install -y build-essential gcc make kmod util-linux gdb ca-certificates linux-headers-$(uname -r)

    mkdir -p /mnt
    chmod 777 /mnt

    echo "Kernel: $(uname -r)"
    if ! uname -r | grep -q '^6\\.12\\.'; then
      echo "WARNING: assignment expects kernel 6.12.x"
    fi
  SHELL
end
