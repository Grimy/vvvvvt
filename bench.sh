#!/bin/sh

i=2000;
while [ "$i" -gt 0 ]; do
	cat tests/*
	i=$((i - 1))
done
