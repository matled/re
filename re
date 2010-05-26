#!/usr/bin/env ruby
# rename edit
# Copyright (c) 2010 Matthias Lederhofer
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, either version 3 of the License, or
# (at your option) any later version.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <http://www.gnu.org/licenses/>.
require 'pathname'
require 'stringio'
require 'tempfile'

class NAdic
  DEFAULT_DIGITS = [0..9, 'A'..'Z'].map(&:to_a).flatten(1).map(&:to_s)

  def initialize(digits = DEFAULT_DIGITS, padding = nil)
    unless digits.length > 1 && digits.select { |s| s.length != 1 }.empty?
      raise ArgumentError, "invalid digits"
    end
    @digits = digits
    @padding = padding || 0
  end

  def pad_for(maximum)
    @padding = to_s(maximum).length
  end

  def to_s(n, padding = @padding)
    raise ArgumentError if n < 0
    result = ""
    while n > 0
      n, index = n.divmod(@digits.length)
      result << @digits[index]
    end
    result = @digits[0] if result.empty?
    if result.length < padding
      result += @digits[0] * (padding - result.length)
    end
    result.reverse
  end

  def to_i(s)
    s.split("").inject(0) do |m, e|
      index = @digits.index(e)
      raise ArgumentError, "invalid digit #{e.inspect}" unless index
      m * @digits.length + index
    end
  end
end

class RenameEdit
  DEFAULT_OPTIONS = {
    :separator => ":",
    :recursive => false,
    :hide_extension => false,
    :verbose => false,
  }

  attr_reader :files, :files_new, :errors
  attr_accessor :separator

  def initialize(files, options = {})
    options = DEFAULT_OPTIONS.merge(options)

    @separator = options[:separator].to_s[0..0]
    @verbose = !!options[:verbose]
    @recursive = !!options[:recursive]
    # TODO: buggy, does not work with ".git", maybe other cases broken too
    @hide_extension = !!options[:hide_extension]

    if @recursive
      @files = files.map { |file| walk(file) }.flatten(1)
    else
      @files = files.map { |file| Pathname.new(file) }
    end

    @nadic = NAdic.new
    @nadic.pad_for(@files.length - 1)

    @errors = []
  end

  # return path and all entries below path as array of Pathname's
  def walk(path)
    path = Pathname.new(path) unless path.is_a?(Pathname)
    return [path] unless path.directory?

    # TODO: error handling (EPERM)
    entries = path.entries.
      select { |p| !%w(. ..).include?(p.to_s) }.
      map { |p| (path + p).directory? ? path + "#{p.to_s}/" : path + p }.
      # TODO: maybe do not put directories at the beginning?  not sure yet
      sort_by { |p| [p.directory? ? 0 : 1, p.basename] }

    if %w(. ..).include?(path.basename.to_s)
      []
    else
      [path]
    end +
      entries +
      entries.select { |p| p.directory? }.
      map { |p| walk(p)[1..-1] }.
      flatten(1)
  end

  def escape(s)
    if s =~ /\A"|\n/
      s.inspect
    else
      s
    end
  end

  def unescape(s)
    # TODO: if s starts with " use eval, use the correct scope!
    s
  end

  def split_extension(str)
    # TODO: handle .tar.gz
    if str =~ /\A(.+)\.([^.]+)\z/
      [$1, $2]
    else
      [str, nil]
    end
  end

  def join_extension(reference, name)
    _, ext = split_extension(reference.basename.to_s)
    if ext
      "#{name}.#{ext}"
    else
      name
    end
  end

  def dump(io = nil)
    orig_io = io
    io ||= StringIO.new

    base = nil
    @files.each_with_index do |file, index|
      unless file.dirname == base
        base = file.dirname
        io.puts "# #{escape file.dirname.to_s + "/"}"
      end

      if file.to_s[-1] == ?/
        name = file.basename.to_s + "/"
      else
        name = file.basename.to_s
        name, = split_extension(name) if @hide_extension
      end
      io.puts [@nadic.to_s(index), @separator, escape(name)].join
    end

    return io.string unless orig_io
  end

  def load(io)
    @files_new = @files.dup
    line = 0
    entry = 0
    io.each do |str|
      line += 1
      str = str.chomp
      next if str =~ /\A\s*(?:#|\z)/
      index, name = str.split(@separator, 2)
      # TODO: handle ArgumentError
      index = @nadic.to_i(index.upcase)
      name = unescape(name)
      # TODO: validate filename (\0 and /)
      # TODO: handle invalid indices
      name = join_extension(@files_new[index], name) if @hide_extension
      unless @files_new[index].basename == name
        @files_new[index] = @files_new[index].dirname + name
      end
    end
  end

  def renames
    @files.zip(@files_new).select do |from, to|
      # TODO: handle trailing slash for directories
      from.to_s.sub(/\/\z/, '') != to.to_s.sub(/\/\z/, '')
    end
  end

  def rename
    # TODO: reverse might be confusing in verbose output.  perhaps try to keep
    # the order as far as possible
    renames.sort_by { |a,b| a }.reverse.each do |from, to|
      if File.exist?(to)
        if block_given?
          next unless yield from, to
        else
          warn "not renaming because target exists: " \
            "#{from.to_s.inspect} -> #{to.to_s.inspect}"
          next
        end
      end

      begin
        File.rename(from, to)
        puts "%s -> %s" % [from, to].map { |p| p.to_s.inspect } if @verbose
      rescue SystemCallError
        @errors << $!
        warn "%s: %s -> %s" %
        # XXX: create a new exception because ruby embeds the
        # filenames into the message
        [SystemCallError.new($!.errno).message,
          from.to_s.inspect, to.to_s.inspect]
      end
    end
  end

  def run
    Tempfile.open(File.basename($0)) do |fh|
      dump(fh)
      fh.flush

      system(ENV["EDITOR"] || "vim", fh.path)
      fh.seek(0)
      load(fh)

      rename
    end
    @errors.empty?
  end
end

if $0 == __FILE__
  require 'optparse'

  options = {}
  begin
    opt = OptionParser.new do |opts|
      opts.on("-v", "--verbose", "verbose output") do |v|
        options[:verbose] = true
      end

      opts.on("-r", "--recursive", "walk paths recursively") do |v|
        options[:recursive] = true
      end

      opts.on("-s", "--separator SEPARATOR",
              "separator between index and file") do |v|
        options[:separator] = v[0..0]
      end
    end.parse!
  rescue OptionParser::ParseError
    warn $!.message
    exit 1
  end

  exit RenameEdit.new(ARGV, options).run
end
