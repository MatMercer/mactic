class Mactic < Formula
  desc "Send haptic waveforms and visualize multitouch input on MacBook trackpads"
  homepage "https://github.com/MatMercer/mactic"
  url "https://github.com/MatMercer/mactic/releases/download/v1/mactic-macos.tar.gz"
  sha256 "TODO"
  license "Unlicense"

  depends_on :macos

  def install
    bin.install "mactic"
  end

  test do
    assert_match "Usage:", shell_output("#{bin}/mactic -h 2>&1", 0)
  end
end
