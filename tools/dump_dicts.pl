#!/usr/bin/perl
# Writes pgFormatter 5.11's dictionaries (set_dicts) as C++ arrays, in their order.
use strict;
use warnings;
use lib "$ARGV[0]/lib";
use pgFormatter::Beautify;
my $b = pgFormatter::Beautify->new();
my $d = $b->{'dict'};
sub arr {
    my ($name, $list) = @_;
    my $out = "    const std::vector<std::string_view> &$name() {\n        static const std::vector<std::string_view> list = {";
    my $line = '';
    my @parts;
    foreach my $w (@$list) {
        die "unexpected character in $w" if $w =~ /["\\]/;
        push @parts, "\"$w\"";
    }
    my $col = 999;
    foreach my $p (@parts) {
        if ($col + length($p) + 2 > 110) { $out .= "\n            "; $col = 12; }
        $out .= " $p,"; $col += length($p) + 2;
    }
    $out .= "\n        };\n        return list;\n    }\n";
    return $out;
}
print "// Generated from pgFormatter 5.11's set_dicts() (lib/pgFormatter/Beautify.pm) by dump_dicts.pl:\n";
print "// the word lists in their order (order matters where a list becomes a regular expression's\n";
print "// alternatives). pg_functions is a hash in pgFormatter, so its order is sorted here.\n";
print "// Copyright (c) 2012-2026, Gilles Darold; PostgreSQL Licence (see NOTICE).\n\n";
print "#include \"dicts.h\"\n\nnamespace sc::pgformat::detail {\n";
print arr('pg_keywords', $d->{'pg_keywords'});
print arr('pg_types', $d->{'pg_types'});
print arr('sql_keywords', $d->{'sql_keywords'});
print arr('redshift_keywords', $d->{'redshift_keywords'});
print arr('pg_functions', [sort keys %{$d->{'pg_functions'}}]);
print arr('copy_keywords', $d->{'copy_keywords'});
print arr('brackets', $d->{'brackets'});
my $sym = $d->{'symbols'};
print "    const std::vector<std::pair<std::string_view, std::string_view>> &symbols() {\n        static const std::vector<std::pair<std::string_view, std::string_view>> list = {\n";
foreach my $k (sort keys %$sym) { print "            {\"$k\", \"$sym->{$k}\"},\n"; }
print "        };\n        return list;\n    }\n";
print "}\n";
printf STDERR "keywords %d types %d sql %d redshift %d functions %d\n", scalar @{$d->{'pg_keywords'}}, scalar @{$d->{'pg_types'}}, scalar @{$d->{'sql_keywords'}}, scalar @{$d->{'redshift_keywords'}}, scalar keys %{$d->{'pg_functions'}};
